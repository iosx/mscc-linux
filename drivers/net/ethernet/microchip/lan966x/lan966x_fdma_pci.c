// SPDX-License-Identifier: GPL-2.0+

#include <linux/bpf_trace.h>

#include "fdma_api.h"
#include "lan966x_main.h"

static int lan966x_fdma_pci_dataptr_cb(struct fdma *fdma, int dcb, int db,
				       u64 *dataptr)
{
	*dataptr = fdma_pci_atu_get_mapped_addr(fdma->atu_region,
						fdma_dataptr_get_contiguous(fdma, dcb, db));

	return 0;
}

static int lan966x_fdma_pci_nextptr_cb(struct fdma *fdma, int dcb, u64 *nextptr)
{
	u64 addr;

	fdma_nextptr_cb(fdma, dcb, &addr);

	*nextptr = fdma_pci_atu_get_mapped_addr(fdma->atu_region, addr);

	return 0;
}

static int lan966x_fdma_pci_rx_alloc(struct lan966x_rx *rx)
{
	struct lan966x *lan966x = rx->lan966x;
	struct fdma *fdma = rx->fdma;
	int err;

	err = fdma_alloc_coherent_and_map(lan966x->dev, fdma, &lan966x->atu);
	if (err)
		return err;

	fdma_dcbs_init(fdma,
		       FDMA_DCB_INFO_DATAL(fdma->db_size),
		       FDMA_DCB_STATUS_INTR);

	lan966x_fdma_llp_configure(lan966x,
				   fdma->atu_region->base_addr,
				   fdma->channel_id);

	return 0;
}

static int lan966x_fdma_pci_tx_alloc(struct lan966x_tx *tx)
{
	struct lan966x *lan966x = tx->lan966x;
	struct fdma *fdma = tx->fdma;
	int err;

	err = fdma_alloc_coherent_and_map(lan966x->dev, fdma, &lan966x->atu);
	if (err)
		return err;

	fdma_dcbs_init(fdma,
		       FDMA_DCB_INFO_DATAL(fdma->db_size),
		       FDMA_DCB_STATUS_DONE);

	lan966x_fdma_llp_configure(lan966x,
				   fdma->atu_region->base_addr,
				   fdma->channel_id);

	return 0;
}

static int lan966x_fdma_pci_rx_check_frame(struct lan966x_rx *rx, u64 *src_port)
{
	struct lan966x *lan966x = rx->lan966x;
	struct fdma *fdma = rx->fdma;
	struct lan966x_port *port;
	struct fdma_db *db;
	void *virt_addr;

	db = &fdma->dcbs[fdma->dcb_index].db[fdma->db_index];

	virt_addr = fdma_dataptr_virt_get_contiguous(fdma,
						     fdma->dcb_index,
						     fdma->db_index);

	lan966x_ifh_get_src_port(virt_addr, src_port);

	if (WARN_ON(*src_port >= lan966x->num_phys_ports))
		return FDMA_ERROR;

	port = lan966x->ports[*src_port];
	if (!lan966x_xdp_port_present(port))
		return FDMA_PASS;

	return lan966x_xdp_pci_run(port,
				   virt_addr,
				   FDMA_DCB_STATUS_BLOCKL(db->status));
}

static struct sk_buff *lan966x_fdma_pci_rx_get_frame(struct lan966x_rx *rx,
						     u64 src_port)
{
	struct lan966x *lan966x = rx->lan966x;
	struct fdma *fdma = rx->fdma;
	struct sk_buff *skb;
	struct fdma_db *db;
	u64 timestamp;

	/* Get the received frame and create an SKB for it */
	db = fdma_db_next_get(fdma);

	skb = __netdev_alloc_skb(rx->lan966x->fdma_ndev,
				 rx->max_mtu,
				 GFP_ATOMIC);
	if (unlikely(!skb))
		goto out;

	skb_reserve(skb, XDP_PACKET_HEADROOM);

	memcpy(skb->data,
	       fdma_dataptr_virt_get_contiguous(fdma, fdma->dcb_index,
						fdma->db_index),
	       rx->max_mtu - XDP_PACKET_HEADROOM);

	skb_put(skb, FDMA_DCB_STATUS_BLOCKL(db->status));

	lan966x_ifh_get_timestamp(skb->data, &timestamp);

	skb->dev = lan966x->ports[src_port]->dev;
	skb_pull(skb, IFH_LEN_BYTES);

	if (likely(!(skb->dev->features & NETIF_F_RXFCS)))
		skb_trim(skb, skb->len - ETH_FCS_LEN);

	lan966x_ptp_rxtstamp(lan966x, skb, src_port, timestamp);
	skb->protocol = eth_type_trans(skb, skb->dev);

	if (lan966x->bridge_mask & BIT(src_port)) {
		skb->offload_fwd_mark = 1;

		skb_reset_network_header(skb);
		if (!lan966x_hw_offload(lan966x, src_port, skb))
			skb->offload_fwd_mark = 0;
	}

	skb->dev->stats.rx_bytes += skb->len;
	skb->dev->stats.rx_packets++;

	return skb;

out:
	return NULL;
}

int lan966x_xdp_pci_setup(struct net_device *dev, struct netdev_bpf *xdp)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct bpf_prog *old_prog;
	bool old_xdp, new_xdp;

	old_xdp = lan966x_xdp_present(lan966x);
	old_prog = xchg(&port->xdp_prog, xdp->prog);
	new_xdp = lan966x_xdp_present(lan966x);

	if (old_xdp == new_xdp && old_prog)
		bpf_prog_put(old_prog);

	return 0;
}

static int lan966x_fdma_pci_get_next_dcb(struct fdma *fdma)
{
	struct fdma_db *db;
	int i;

	for (i = 0; i < fdma->n_dcbs; i++) {
		db = fdma_db_get(fdma, i, 0);

		if (!unlikely(fdma_db_is_done(db)))
			continue;
		if (fdma_is_last(fdma, &fdma->dcbs[i]))
			continue;

		return i;
	}

	return -1;
}

static int lan966x_fdma_pci_xmit_xdpf(struct lan966x_port *port, void *ptr,
				      u32 len)
{
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_tx *tx = &lan966x->tx;
	struct fdma *fdma = tx->fdma;
	int next_to_use, ret = 0;
	void *virt_addr;
	__be32 *ifh;

	spin_lock(&lan966x->tx_lock);

	next_to_use = lan966x_fdma_pci_get_next_dcb(fdma);

	if (next_to_use < 0) {
		netif_stop_queue(port->dev);
		ret = NETDEV_TX_BUSY;
		goto out;
	}

	ifh = ptr;
	memset(ifh, 0, IFH_LEN_BYTES);
	lan966x_ifh_set_bypass(ifh, 1);
	lan966x_ifh_set_port(ifh, BIT_ULL(port->chip_port));

	/* Get the virtual addr of the next DB and copy frame incl. IFH to it.*/
	virt_addr = fdma_dataptr_virt_get_contiguous(fdma, fdma->dcb_index, 0);
	memcpy(virt_addr, ptr, len);

	fdma_dcb_add(fdma,
		     fdma->dcb_index,
		     0,
		     FDMA_DCB_STATUS_SOF |
		     FDMA_DCB_STATUS_EOF |
		     FDMA_DCB_STATUS_BLOCKO(0) |
		     FDMA_DCB_STATUS_BLOCKL(len));

	/* Start the transmission */
	lan966x_fdma_tx_start(tx);

out:
	spin_unlock(&lan966x->tx_lock);

	return ret;
}

int lan966x_xdp_pci_run(struct lan966x_port *port, void *data, u32 data_len)
{
	struct bpf_prog *xdp_prog = port->xdp_prog;
	struct lan966x *lan966x = port->lan966x;
	struct xdp_buff xdp;
	u32 act;

	xdp_init_buff(&xdp, lan966x->rx.max_mtu, &port->xdp_rxq);

	xdp_prepare_buff(&xdp,
			 data,
			 IFH_LEN_BYTES + XDP_PACKET_HEADROOM,
			 data_len - IFH_LEN_BYTES,
			 false);

	act = bpf_prog_run_xdp(xdp_prog, &xdp);
	switch (act) {
	case XDP_PASS:
		return FDMA_PASS;
	case XDP_TX:
		return lan966x_fdma_pci_xmit_xdpf(port, data, data_len) ?
		       FDMA_DROP : FDMA_TX;
	default:
		bpf_warn_invalid_xdp_action(port->dev, xdp_prog, act);
		fallthrough;
	case XDP_ABORTED:
		trace_xdp_exception(port->dev, xdp_prog, act);
		fallthrough;
	case XDP_DROP:
		return FDMA_DROP;
	}
}

static int lan966x_fdma_pci_xmit(struct sk_buff *skb, __be32 *ifh,
				 struct net_device *dev)
{
	struct lan966x_port *port = netdev_priv(dev);
	struct lan966x *lan966x = port->lan966x;
	struct lan966x_tx *tx = &lan966x->tx;
	struct fdma *fdma = tx->fdma;
	bool ptp = false;
	int next_to_use;
	void *virt_addr;

	next_to_use = lan966x_fdma_pci_get_next_dcb(fdma);

	if (next_to_use < 0) {
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}

	if (skb_put_padto(skb, ETH_ZLEN)) {
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	skb_tx_timestamp(skb);

	virt_addr = fdma_dataptr_virt_get_contiguous(fdma, next_to_use, 0);
	memcpy(virt_addr, ifh, IFH_LEN_BYTES);
	memcpy((u8 *)virt_addr + IFH_LEN_BYTES, skb->data, skb->len);

	fdma_dcb_add(fdma, next_to_use, 0,
		     FDMA_DCB_STATUS_SOF |
		     FDMA_DCB_STATUS_EOF |
		     FDMA_DCB_STATUS_BLOCKO(0) |
		     FDMA_DCB_STATUS_BLOCKL(IFH_LEN_BYTES + skb->len + ETH_FCS_LEN));

	if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP &&
	    LAN966X_SKB_CB(skb)->rew_op == IFH_REW_OP_TWO_STEP_PTP)
		ptp = true;

	/* Start the transmission */
	lan966x_fdma_tx_start(tx);

	if (!ptp)
		dev_consume_skb_any(skb);

	return NETDEV_TX_OK;
}

static int lan966x_fdma_pci_napi_poll(struct napi_struct *napi, int weight)
{
	struct lan966x *lan966x = container_of(napi, struct lan966x, napi);
	struct lan966x_rx *rx = &lan966x->rx;
	struct fdma *fdma = rx->fdma;
	int dcb_reload, old_dcb;
	struct sk_buff *skb;
	int counter = 0;
	u64 src_port;

	dcb_reload = fdma->dcb_index;

	/* Get all received skb */
	while (counter < weight) {
		if (!fdma_has_frames(fdma))
			break;
		counter++;
		switch (lan966x_fdma_pci_rx_check_frame(rx, &src_port)) {
		case FDMA_PASS:
			break;
		case FDMA_ERROR:
			fdma_dcb_advance(fdma);
			goto allocate_new;
		case FDMA_TX:
			fdma_dcb_advance(fdma);
			continue;
		case FDMA_DROP:
			fdma_dcb_advance(fdma);
			continue;
		}
		skb = lan966x_fdma_pci_rx_get_frame(rx, src_port);
		if (!skb)
			goto allocate_new;

		/* Check if the DCB can be reused */
		fdma_db_advance(fdma);
		if (fdma->db_index != fdma->n_dbs) {
			napi_gro_receive(&lan966x->napi, skb);
			continue;
		}

		fdma_db_reset(fdma);
		fdma_dcb_advance(fdma);

		napi_gro_receive(&lan966x->napi, skb);
	}
allocate_new:
	while (dcb_reload != fdma->dcb_index) {
		old_dcb = dcb_reload;
		dcb_reload++;
		dcb_reload &= fdma->n_dcbs - 1;

		fdma_dcb_add(fdma,
			     old_dcb,
			     FDMA_DCB_INFO_DATAL(fdma->db_size),
			     FDMA_DCB_STATUS_INTR);

		lan966x_fdma_rx_reload(rx);
	}

	if (counter < weight && napi_complete_done(napi, counter))
		lan_wr(0xff, lan966x, FDMA_INTR_DB_ENA);

	return counter;
}

/* Reset existing rx and tx buffers */
static void lan966x_fdma_pci_reset_mem(struct lan966x *lan966x)
{
	struct lan966x_rx *rx = &lan966x->rx;
	struct lan966x_tx *tx = &lan966x->tx;

	memset(rx->fdma->dcbs, 0, rx->fdma->size);
	memset(tx->fdma->dcbs, 0, tx->fdma->size);

	fdma_dcbs_init(rx->fdma,
		       FDMA_DCB_INFO_DATAL(rx->fdma->db_size),
		       FDMA_DCB_STATUS_INTR);

	fdma_dcbs_init(tx->fdma,
		       FDMA_DCB_INFO_DATAL(tx->fdma->db_size),
		       FDMA_DCB_STATUS_DONE);

	lan966x_fdma_llp_configure(lan966x,
				   tx->fdma->atu_region->base_addr,
				   tx->fdma->channel_id);
	lan966x_fdma_llp_configure(lan966x,
				   rx->fdma->atu_region->base_addr,
				   rx->fdma->channel_id);
}

static int lan966x_fdma_pci_reload(struct lan966x *lan966x, int new_mtu)
{
	struct fdma tx_fdma_old = *lan966x->tx.fdma;
	struct fdma rx_fdma_old = *lan966x->rx.fdma;
	u32 old_mtu = lan966x->rx.max_mtu;
	int err;

	napi_synchronize(&lan966x->napi);
	napi_disable(&lan966x->napi);
	lan966x_fdma_stop_netdev(lan966x);
	lan966x_fdma_rx_disable(&lan966x->rx);

	lan966x->rx.max_mtu = new_mtu;

	lan966x->tx.fdma->db_size = FDMA_PCI_DB_SIZE(lan966x->rx.max_mtu);
	lan966x->tx.fdma->size = fdma_get_size_contiguous(lan966x->tx.fdma);
	lan966x->rx.fdma->db_size = FDMA_PCI_DB_SIZE(lan966x->rx.max_mtu);
	lan966x->rx.fdma->size = fdma_get_size_contiguous(lan966x->rx.fdma);

	err = lan966x_fdma_pci_rx_alloc(&lan966x->rx);
	if (err)
		goto restore;

	err = lan966x_fdma_pci_tx_alloc(&lan966x->tx);
	if (err) {
		fdma_free_coherent_and_unmap(lan966x->dev, lan966x->rx.fdma);
		goto restore;
	}

	lan966x_fdma_rx_start(&lan966x->rx);

	/* Free and unmap old memory */
	fdma_free_coherent_and_unmap(lan966x->dev, &rx_fdma_old);
	fdma_free_coherent_and_unmap(lan966x->dev, &tx_fdma_old);

	lan966x_fdma_wakeup_netdev(lan966x);
	napi_enable(&lan966x->napi);

	return err;
restore:

	/* No new buffers are allocated at this point. Use the old buffers,
	 * but reset them before starting the FDMA again.
	 */

	memcpy(lan966x->tx.fdma, &tx_fdma_old, sizeof(struct fdma));
	memcpy(lan966x->rx.fdma, &rx_fdma_old, sizeof(struct fdma));

	lan966x->rx.max_mtu = old_mtu;

	lan966x_fdma_pci_reset_mem(lan966x);

	lan966x_fdma_rx_start(&lan966x->rx);
	lan966x_fdma_wakeup_netdev(lan966x);
	napi_enable(&lan966x->napi);

	return err;
}

static int __lan966x_fdma_pci_reload(struct lan966x *lan966x, int max_mtu)
{
	int err;
	u32 val;

	/* Disable the CPU port */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(0),
		QSYS_SW_PORT_MODE_PORT_ENA,
		lan966x, QSYS_SW_PORT_MODE(CPU_PORT));

	/* Flush the CPU queues */
	readx_poll_timeout(lan966x_qsys_sw_status, lan966x,
			   val, !(QSYS_SW_STATUS_EQ_AVAIL_GET(val)),
			   READL_SLEEP_US, READL_TIMEOUT_US);

	/* Add a sleep in case there are frames between the queues and the CPU
	 * port
	 */
	usleep_range(1000, 2000);

	err = lan966x_fdma_pci_reload(lan966x, max_mtu);

	/* Enable back the CPU port */
	lan_rmw(QSYS_SW_PORT_MODE_PORT_ENA_SET(1),
		QSYS_SW_PORT_MODE_PORT_ENA,
		lan966x,  QSYS_SW_PORT_MODE(CPU_PORT));

	return err;
}

static int lan966x_fdma_pci_change_mtu(struct lan966x *lan966x)
{
	int max_mtu;

	max_mtu = lan966x_fdma_get_max_frame(lan966x);
	if (max_mtu == lan966x->rx.max_mtu)
		return 0;

	return __lan966x_fdma_pci_reload(lan966x, max_mtu);
}

static struct fdma lan966x_fdma_tx = {
	.channel_id = FDMA_INJ_CHANNEL,
	.n_dcbs = 1024,
	.n_dbs = 1,
	.ops = {
		.dataptr_cb = &lan966x_fdma_pci_dataptr_cb,
		.nextptr_cb = &lan966x_fdma_pci_nextptr_cb,
	},
};

static struct fdma lan966x_fdma_rx = {
	.channel_id = FDMA_XTR_CHANNEL,
	.n_dcbs = FDMA_DCB_MAX * 2,
	.n_dbs = 1,
	.ops = {
		.dataptr_cb = &lan966x_fdma_pci_dataptr_cb,
		.nextptr_cb = &lan966x_fdma_pci_nextptr_cb,
	},
};

static int lan966x_fdma_pci_init(struct lan966x *lan966x)
{
	int err;

	if (!lan966x->fdma)
		return 0;

	/* The data blocks must be placed on 128-byte word aligned addresses in
	 * memory, and their length must be a multiple of 128 bytes.
	 */
	fdma_pci_atu_init(&lan966x->atu, lan966x->regs[TARGET_PCIE_DBI]);

	lan966x->rx.lan966x = lan966x;
	lan966x->rx.max_mtu = lan966x_fdma_get_max_frame(lan966x);
	lan966x->rx.fdma = &lan966x_fdma_rx;
	lan966x->rx.fdma->db_size = FDMA_PCI_DB_SIZE(lan966x->rx.max_mtu);
	lan966x->rx.fdma->size = fdma_get_size_contiguous(lan966x->rx.fdma);
	lan966x->tx.lan966x = lan966x;
	lan966x->tx.fdma = &lan966x_fdma_tx;
	lan966x->tx.fdma->db_size = FDMA_PCI_DB_SIZE(lan966x->rx.max_mtu);
	lan966x->tx.fdma->size = fdma_get_size_contiguous(lan966x->tx.fdma);

	err = lan966x_fdma_pci_rx_alloc(&lan966x->rx);
	if (err)
		return err;

	err = lan966x_fdma_pci_tx_alloc(&lan966x->tx);
	if (err) {
		fdma_free_coherent_and_unmap(lan966x->dev, lan966x->rx.fdma);
		return err;
	}

	lan966x_fdma_rx_start(&lan966x->rx);

	return 0;
}

static void lan966x_fdma_pci_deinit(struct lan966x *lan966x)
{
	if (!lan966x->fdma)
		return;

	lan966x_fdma_rx_disable(&lan966x->rx);
	lan966x_fdma_tx_disable(&lan966x->tx);

	napi_synchronize(&lan966x->napi);
	napi_disable(&lan966x->napi);

	fdma_free_coherent_and_unmap(lan966x->dev, lan966x->rx.fdma);
	fdma_free_coherent_and_unmap(lan966x->dev, lan966x->tx.fdma);
}

const struct lan966x_match_data lan966x_pci_desc = {
	.ops = {
		.fdma_init = &lan966x_fdma_pci_init,
		.fdma_deinit = &lan966x_fdma_pci_deinit,
		.fdma_xmit = &lan966x_fdma_pci_xmit,
		.fdma_poll = &lan966x_fdma_pci_napi_poll,
		.fdma_mtu = &lan966x_fdma_pci_change_mtu,
		.xdp_setup = &lan966x_xdp_pci_setup,
	},
};
