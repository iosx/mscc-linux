// SPDX-License-Identifier: GPL-2.0+
/* Microchip Sparx5 Switch driver
 *
 * Copyright (c) 2024 Microchip Technology Inc. and its subsidiaries.
 */

#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/list.h>
#include <linux/rhashtable.h>
#include <net/addrconf.h>
#include <net/arp.h>
#include <net/fib_notifier.h>
#include <net/ip6_fib.h>
#include <net/ipv6.h>
#include <net/ndisc.h>
#include <net/neighbour.h>
#include <net/netevent.h>
#include <net/nexthop.h>

#include "sparx5_main.h"
#include "sparx5_port.h"

/* The main routing objects are
 *
 * 1) Router legs, which correspond to IP interfaces. They can have multiple IPs
 *    attached and are associated with a VLAN. Only routes with nexthops that
 *    egress on ports that are part of a router leg are considered for
 *    offloading.
 *
 * 2) Fib entries, which correspond to entries in the routing table.
 *    > ip route add 6.6.0.0/16 nexthop via 1.0.10.10
 *    will create fib entry for 6.6.0.0/16, if it is offloadable.
 *
 * 3) Nexthop groups and nexthops. Each fib entry has exactly one nexthop hop.
 *    To simplify the SW representation, we duplicate nexthops and nexthops
 *    groups for each fib entry, even when the same nexthop is used for many
 *    different routes.
 *
 * 4) Neigh entries, which correspond to directly connected neighbours. These are
 *    created mainly by ARP events. Their job is to maintain the association of
 *    L3 and L2 addressses.
 *
 *    The neigh entries are referenced by nexthops and fib entries. They are
 *    shared so we must keep track of the objects that are referencing them.
 *
 *    If a neighbour which is used in a nexthop group dies, we will set the mac
 *    to zero, so traffic for this nexthop is trapped.
 *
 *
 * Both fib_entry and neigh_entry can trigger writes to the LPM VCAP, and own
 * entries in HW. Fib entries own the corresponding route associated with them.
 * Neigh entries own a /32 route, for traffic destined directly to the neighbour.
 *
 * We have 3 main cases for routing:
 *
 * 1) Routes for directly connected subnets. E.g. router has IP 1.0.10.1, and
 *    routes subnet 1.0.10.0/24.
 *
 *    In this case, we have a fib_entry, with a non-gateway nexthop group. We
 *    install a LPM VCAP route, with action type arp entry, for 1.0.10.0/24 with
 *    a zero mac, to ensure frames destined for this subnet will be sent to the
 *    CPU and start the ARP process.
 *
 *    When we get the arp reply, we create a neigh_entry for each neighbour and
 *    install a direct route in the LPM VCAP for this neighbour. For example,
 *    1.0.10.11 is sent to DMAC 0x1111110001. This is how we route directly
 *    connected subnets.
 *
 *    Moreover, the fib_entry maintains a list of all neighbours discovered in
 *    the subnet it is routing. These neighbours hold a reference back to the
 *    fib_entry.
 *
 *                   |-> neigh_entry 1.0.10.10
 *        neigh_list |-> neigh_entry 1.0.10.11
 *                   |-> neigh_entry 1.0.10.12
 *                   |
 *   +-------------+ |  +--------------+
 *   |  fib_entry  |-+  | nexthop group|
 *   | 1.0.10.1/24 |----+              |
 *   +-------------+    |  nexthop --------> NULL (Write zero mac in VCAP)
 *                      +--------------+
 *
 *
 * 2) Routes for non-connected subnets. E.g. we are routing subnet 6.6.0.0/16,
 *    but we have no IP in this subnet. We are routing via nexthops, which are
 *    directly connected. Say we have >=2 nexthops.
 *
 *    In this case, have a fib_entry with a gateway nexthop group. Each nexthop
 *    points to a neigh_entry, corresponding to the gateway used for routing.
 *
 *    Say we use the nexthops:
 *
 *    - 1.0.11.10
 *    - 1.0.10.10
 *    - 1.0.9.10
 *
 *    We install a LPM VCAP route for 6.6.0.0/16, which contains a pointer to the
 *    hw arp table, and the size of the group. The ARP table contains the Mac
 *    addresses of the nexthops. The Mac addresses are supplied by neigh_entries.
 *
 *
 *   +-------------+    +--------------+
 *   |  fib_entry  |    | nexthop group|
 *   | 6.6.0.0/164 |----+              |
 *   +-------------+    |  nexthop --------> neigh_entry 1.0.11.10
 *                      |  nexthop --------> neigh_entry 1.0.10.10
 *                      |  nexthop --------> neigh_entry 1.0.9.10
 *                      +--------------+
 *
 *
 * 3) Local routes for traffic destined to the router. If the router has an IP
 *    1.0.10.1, then we must ensure this traffic is sent to the CPU.
 *    Therefore, we install a direct route for 1.0.10.1/32 in the LPM VCAP, with
 *    zero mac.
 *
 * 4) All IPv6 link-local traffic is explicitly trapped.
 *
 * On the hardware side, we use the VCAP LPM and ARP table for UC IPv4 routing.
 * HW picks the match found at the highest address in the VCAP LPM. To ensure the
 * longest prefix match we make sure to order the entries according to mask
 * length, with longer masks at higher addresses.
 *
 * It is possible to store ARP data, such as DMAC, directly in the VCAP LPM using
 * ARP entry actions. We do this whenever possible, so the ARP table is only used
 * when a route has multiple nexthops.
 *
 * With the above breakdown in mind, cases 1) and 3) use arp entries, and case 2)
 * use the arp table if the number of nexthops is >1.
 *
 * If the DMAC written to HW is all zero, the chip will trap the frame,
 * redirecting it to the CPU. This is how we get the kernel to perform ARP
 * requests on our behalf.
 *
 * The nexthop group must be laid out at contiguous addresses in the ARP table.
 * The VCAP LPM stores a pointer to the bottom address in the group, and the
 * group size. We do not use the arp pointer remap table.
 *
 * The layout of nexthops in a nexthop group matches the layout in HW, e.g.
 *
 * nhgi->nexthops[0] -> arp table address n
 * ...
 * nhgi->nexthops[k] -> arp table address n+k
 *
 * where the n is the ARP table offset (atbl_offset) for the group.
 */

#define SPARX5_MAX_ECMP_SIZE 16
#define SPARX5_RLEG_USE_GLOBAL_BASE_MAC 2
#define SPARX5_LINK_LOCAL_PREFIX_LEN 64

struct sparx5_rr_fib6_entry_info {
	struct fib6_info **rt_arr;
	unsigned int nrt6;
};

enum sparx5_rr_l3_version {
	SPARX5_IPV4 = 0,
	SPARX5_IPV6,
};

#define SPARX5_IADDR_LEN(v) ((v) == SPARX5_IPV4 ? 32 : 128)

struct sparx5_rr_fib_info {
	union {
		struct fib_entry_notifier_info fen4_info;
		struct sparx5_rr_fib6_entry_info fe6_info;
	};
	enum sparx5_rr_l3_version version;
};

struct sparx5_fib_event_work {
	struct work_struct work;
	struct sparx5_rr_fib_info fi;
	struct sparx5 *sparx5;
	unsigned long event;
};

struct sparx5_rr_netevent_work {
	struct work_struct work;
	struct sparx5 *sparx5;
	struct neighbour *neigh;
	unsigned long event;
};

struct sparx5_rr_router_leg {
	struct net_device *dev;
	struct sparx5 *sparx5;
	struct list_head leg_list_node; /* Router member */
	u16 vmid; /* Internal id */
	u32 vid; /* VLAN id */
};

struct sparx5_iaddr {
	union {
		__be32 ipv4;
		struct in6_addr ipv6;
	}; /* Must be first */
	enum sparx5_rr_l3_version version;
};

struct sparx5_rr_neigh_entry {
	struct sparx5_rr_neigh_key {
		struct net_device *dev;
		struct sparx5_iaddr iaddr;
	} key;
	struct rhash_head ht_node;
	struct sparx5_rr_fib_entry *fib_entry;
	struct list_head fib_list_node; /* Fib route for this neighbour */
	struct neigh_table *neigh_tbl; /* Kernel neighbour table */
	struct list_head nexthop_list; /* Nexthops using this neigh entry */
	struct sparx5_rr_hw_route hw_route;
	unsigned char hwaddr[ETH_ALEN];
	u16 vmid;
	bool connected;
};

struct sparx5_rr_nexthop {
	struct sparx5_rr_neigh_entry *neigh_entry;
	struct sparx5_rr_nexthop_group *grp;
	struct list_head neigh_list_node; /* Neigh entry member */
	struct list_head leg_list_node; /* Router leg member */
	struct neigh_table *neigh_tbl; /* Kernel neighbour table */
	struct sparx5_iaddr gw_addr;
	int ifindex;
	bool gateway;
	bool trapped;
};

struct sparx5_rr_nexthop_group_info {
	struct sparx5_rr_nexthop_group *grp;
	u16 atbl_offset;
	bool atbl_offset_valid;
	u8 count; /* HW allows up to 16 nexthops */
	struct sparx5_rr_nexthop nexthops[] __counted_by(count);
};

struct sparx5_rr_nexthop_group {
	struct sparx5_rr_fib_entry *fib_entry;
	struct sparx5_rr_nexthop_group_info *nhgi;
};

enum sparx5_rr_fib_type {
	SPARX5_RR_FIB_TYPE_INVALID = 0,
	SPARX5_RR_FIB_TYPE_LOCAL,
	SPARX5_RR_FIB_TYPE_UNICAST,
	SPARX5_RR_FIB_TYPE_MULTICAST,
	SPARX5_RR_FIB_TYPE_BLACKHOLE,
	SPARX5_RR_FIB_TYPE_PROHIBIT,
	SPARX5_RR_FIB_TYPE_UNREACHABLE,
};

struct sparx5_rr_fib_key {
	struct sparx5_iaddr addr;
	u32 prefix_len;
	u32 tb_id; /* Routing table type: RT_TABLE_* */
};

struct sparx5_rr_fib_entry {
	struct sparx5_rr_fib_key key;
	enum sparx5_rr_fib_type type;
	struct rhash_head ht_node; /* Router member */
	struct list_head fib_lpm_node; /* Router member */
	struct list_head neigh_list; /* Neighbours under this route */
	struct sparx5_rr_hw_route hw_route;
	struct sparx5_rr_nexthop_group *nh_grp;
	struct sparx5_rr_fib_info fi;
	u64 sort_key; /* For sw lpm lookup */
	bool trap;
	bool offload_fail;
};

struct sparx5_rr_inet6addr_event_work {
	struct work_struct work;
	struct sparx5 *sparx5;
	struct net_device *dev;
	unsigned long event;
};

static int sparx5_rr_addr_fmt(struct sparx5_iaddr *addr, char *buf, size_t len)
{
	switch (addr->version) {
	case SPARX5_IPV4:
		return snprintf(buf, len, "%pI4b", addr);
	case SPARX5_IPV6:
		return snprintf(buf, len, "%pI6c", addr);
	default:
		WARN_ON(1);
		return snprintf(buf, len, "N/A");
	}
}

static void sparx5_rr_neigh_debug(struct sparx5 *sparx5,
				  struct sparx5_rr_neigh_entry *entry,
				  char *msg)
{
	char nip[INET6_ADDRSTRLEN];

	sparx5_rr_addr_fmt(&entry->key.iaddr, nip, ARRAY_SIZE(nip));

	dev_dbg(sparx5->dev,
		"Neigh entry %s vmid=%u mac=%pM ip=%s connected=%d\n", msg,
		entry->vmid, entry->hwaddr, nip, entry->connected);
}

static void sparx5_rr_fib_link_debug(struct sparx5 *sparx5,
				     struct sparx5_rr_neigh_entry *entry,
				     struct sparx5_rr_fib_entry *fib_entry)
{
	char nip[INET6_ADDRSTRLEN], fip[INET6_ADDRSTRLEN];

	sparx5_rr_addr_fmt(&entry->key.iaddr, nip, ARRAY_SIZE(nip));
	sparx5_rr_addr_fmt(&fib_entry->key.addr, fip, ARRAY_SIZE(fip));

	dev_dbg(sparx5->dev, "Fib link fib=%s/%u -> neigh=%s mac=%pM\n", fip,
		fib_entry->key.prefix_len, nip, entry->hwaddr);
}

static void sparx5_rr_fib_debug(struct sparx5 *sparx5,
				struct sparx5_rr_fib_entry *fib_entry,
				char *msg)
{
	char fip[INET6_ADDRSTRLEN];

	sparx5_rr_addr_fmt(&fib_entry->key.addr, fip, ARRAY_SIZE(fip));

	dev_dbg(sparx5->dev, "Fib entry %s ip=%s/%d type=%d nhs=%u\n", msg, fip,
		fib_entry->key.prefix_len, fib_entry->type,
		fib_entry->nh_grp->nhgi->count);
}

static void sparx5_rr_schedule_work(struct sparx5 *sparx5,
				    struct work_struct *work)
{
	queue_work(sparx5->router->sparx5_router_owq, work);
}

static void sparx5_rr_fib_info_init(struct sparx5_rr_fib_info *fi,
				    enum sparx5_rr_l3_version version)
{
	fi->version = version;

	switch (version) {
	case SPARX5_IPV4:
		fi->fen4_info.fi = NULL;
		return;
	case SPARX5_IPV6:
		fi->fe6_info.nrt6 = 0;
		fi->fe6_info.rt_arr = NULL;
		return;
	}
}

/* Return number of nexthops. */
static int sparx5_rr_fib_info_nhs(struct sparx5_rr_fib_info *fi)
{
	switch (fi->version) {
	case SPARX5_IPV4:
		return fib_info_num_path(fi->fen4_info.fi);
	case SPARX5_IPV6:
		return fi->fe6_info.nrt6;
	default:
		WARN_ON(1);
		return 0;
	}
}

static struct fib_nh_common *
sparx5_rr_fib_info_nhc(struct sparx5_rr_fib_info *fi, int nhsel)
{
	switch (fi->version) {
	case SPARX5_IPV4:
		return fib_info_nhc(fi->fen4_info.fi, nhsel);
	case SPARX5_IPV6:
		return &fi->fe6_info.rt_arr[nhsel]->fib6_nh->nh_common;
	default:
		WARN_ON(1);
		return NULL;
	}
}

static bool sparx5_rr_fib_info_is_nh_obj(struct sparx5_rr_fib_info *fi)
{
	switch (fi->version) {
	case SPARX5_IPV4:
		return !!fi->fen4_info.fi->nh;
	case SPARX5_IPV6:
		return !!fi->fe6_info.rt_arr[0]->nh;
	default:
		WARN_ON(1);
		return false;
	}
}

static u8 sparx5_rr_fib_info_type(struct sparx5_rr_fib_info *fi)
{
	switch (fi->version) {
	case SPARX5_IPV4:
		return fi->fen4_info.type;
	case SPARX5_IPV6:
		return fi->fe6_info.rt_arr[0]->fib6_type;
	default:
		WARN_ON(1);
		return RTN_UNSPEC;
	}
}

static u32 sparx5_rr_fib_info_tb_id(struct sparx5_rr_fib_info *fi)
{
	switch (fi->version) {
	case SPARX5_IPV4:
		return fi->fen4_info.tb_id;
	case SPARX5_IPV6:
		return fi->fe6_info.rt_arr[0]->fib6_table->tb6_id;
	default:
		WARN_ON(1);
		return RT_TABLE_UNSPEC;
	}
}

static bool sparx5_rr_fib6_rt_should_ignore(struct fib6_info *rt)
{
	int addr_type = ipv6_addr_type(&rt->fib6_dst.addr);

	if (addr_type & (IPV6_ADDR_MULTICAST | IPV6_ADDR_LINKLOCAL))
		return true;

	return false;
}

static bool sparx5_rr_fib_info_should_ignore(struct sparx5_rr_fib_info *fi)
{
	struct fib6_info *rt;

	if (fi->version == SPARX5_IPV4)
		return false;

	rt = fi->fe6_info.rt_arr[0];

	return sparx5_rr_fib6_rt_should_ignore(rt);
}

#if IS_ENABLED(CONFIG_IPV6)
static void sparx5_rr_rt6_release(struct fib6_info *rt)
{
	if (!rt->nh)
		rt->fib6_nh->fib_nh_flags &= ~RTNH_F_OFFLOAD;

	fib6_info_release(rt);
}
#else
static void sparx5_rr_rt6_release(struct fib6_info *rt)
{
}
#endif

static void sparx5_rr_fib6_info_put(struct sparx5_rr_fib6_entry_info *fi)
{
	for (int i = 0; i < fi->nrt6; i++)
		sparx5_rr_rt6_release(fi->rt_arr[i]);

	kfree(fi->rt_arr);
	fi->nrt6 = 0;
	fi->rt_arr = NULL;
}

static void sparx5_rr_fib_info_put(struct sparx5_rr_fib_info *fi)
{
	if (fi->version == SPARX5_IPV4) {
		if (fi->fen4_info.fi) {
			fib_info_put(fi->fen4_info.fi);
			fi->fen4_info.fi = NULL;
		}
		return;
	}

	sparx5_rr_fib6_info_put(&fi->fe6_info);
}

static void sparx5_rr_split_mac(unsigned char mac[ETH_ALEN], u32 split,
				u32 *msb, u32 *lsb)
{
	u32 mask = GENMASK(split - 1, 0);
	u64 m = ether_addr_to_u64(mac);

	*lsb = m & mask;
	*msb = m >> split;
}

static int sparx5_rr_arp_tbl_grp_alloc(struct sparx5 *sparx5,
				       unsigned int nh_grp_size)
{
	int offset;

	offset = bitmap_find_next_zero_area(sparx5->router->arp_tbl_mask,
					    sparx5->data->consts.arp_tbl_cnt, 0,
					    nh_grp_size, 0);
	if (offset >= sparx5->data->consts.arp_tbl_cnt)
		return -ENOMEM;

	bitmap_set(sparx5->router->arp_tbl_mask, offset, nh_grp_size);

	return offset;
}

static void sparx5_rr_arp_tbl_grp_free(struct sparx5 *sparx5,
				       unsigned int nh_grp_size, int offset)
{
	bitmap_clear(sparx5->router->arp_tbl_mask, offset, nh_grp_size);
}

static int sparx5_vmid_alloc(struct sparx5 *sparx5)
{
	int vmid;

	vmid = find_first_zero_bit(sparx5->router->vmid_mask,
				   sparx5->data->consts.vmid_cnt);
	if (vmid >= sparx5->data->consts.vmid_cnt)
		return -ENOMEM;

	set_bit(vmid, sparx5->router->vmid_mask);

	return vmid;
}

static void sparx5_vmid_free(struct sparx5 *sparx5, u16 vmid)
{
	clear_bit(vmid, sparx5->router->vmid_mask);
}

static void sparx5_rr_nb2neigh_key(struct neighbour *n,
				   struct sparx5_rr_neigh_key *key)
{
	memset(key, 0, sizeof(*key));

	/* The primary_key, tbl->family and dev are constant for the lifetime of
	 * the neighbour, so we can read them without n->lock.
	 */
	if (n->tbl->family == AF_INET) {
		key->iaddr.version = SPARX5_IPV4;
		key->iaddr.ipv4 = *(__be32 *)n->primary_key;
	} else {
		key->iaddr.version = SPARX5_IPV6;
		key->iaddr.ipv6 = *(struct in6_addr *)n->primary_key;
	}

	key->dev = n->dev;
}

static void
sparx5_rr_neigh_entry_offload_mark(struct sparx5_rr_neigh_entry *entry,
				   bool offloaded)
{
	struct neighbour *n;

	if (!entry->neigh_tbl)
		return;

	n = neigh_lookup(entry->neigh_tbl, &entry->key.iaddr.ipv4,
			 entry->key.dev);
	if (!n)
		return;

	if (offloaded)
		n->flags |= NTF_OFFLOADED;
	else
		n->flags &= ~NTF_OFFLOADED;

	neigh_release(n);
}

static const struct rhashtable_params sparx5_neigh_ht_params = {
	.key_offset = offsetof(struct sparx5_rr_neigh_entry, key),
	.head_offset = offsetof(struct sparx5_rr_neigh_entry, ht_node),
	.key_len = sizeof(struct sparx5_rr_neigh_key),
	.automatic_shrinking = true,
};

static const struct rhashtable_params sparx5_rr_fib_entry_ht_params = {
	.key_offset = offsetof(struct sparx5_rr_fib_entry, key),
	.head_offset = offsetof(struct sparx5_rr_fib_entry, ht_node),
	.key_len = sizeof(struct sparx5_rr_fib_key),
	.automatic_shrinking = true,
};

static bool sparx5_rr_iaddr_equal(struct sparx5_iaddr *a1,
				  struct sparx5_iaddr *a2)
{
	if (a1->version != a2->version)
		return false;

	if (a1->version == SPARX5_IPV4)
		return a1->ipv4 == a2->ipv4;

	return !ipv6_addr_cmp(&a1->ipv6, &a2->ipv6);
}

static u16 sparx5_rr_iaddr_proto(struct sparx5_iaddr *iaddr)
{
	switch (iaddr->version) {
	case SPARX5_IPV4:
		return ETH_P_IP;
	case SPARX5_IPV6:
		return ETH_P_IPV6;
	default:
		WARN_ON(1);
		return 0;
	}
}

static bool
sparx5_rr_nexthop_group_has_nexthop(struct sparx5_rr_nexthop_group *nh_grp,
				    struct sparx5_iaddr *addr, int ifindex)
{
	for (u8 i = 0; i < nh_grp->nhgi->count; i++) {
		struct sparx5_rr_nexthop *nh;

		nh = &nh_grp->nhgi->nexthops[i];
		if (nh->ifindex == ifindex &&
		    sparx5_rr_iaddr_equal(&nh->gw_addr, addr))
			return true;
	}

	return false;
}

static void sparx5_rr_fib_nhc_to_gw(struct fib_nh_common *nhc,
				    enum sparx5_rr_l3_version version,
				    struct sparx5_iaddr *gw)
{
	gw->version = version;

	switch (version) {
	case SPARX5_IPV4:
		gw->ipv4 = nhc->nhc_gw.ipv4;
		break;
	case SPARX5_IPV6:
		gw->ipv6 = nhc->nhc_gw.ipv6;
		break;
	}
}

/* Return true iff fi contains exactly the same nexthops as nh_grp, modulo the
 * order of nexthops.
 */
static bool
sparx5_rr_fib_info_nh_group_equal(struct sparx5_rr_nexthop_group *nh_grp,
				  struct sparx5_rr_fib_info *fi)
{
	u8 nhs = sparx5_rr_fib_info_nhs(fi);
	struct sparx5_iaddr gw = { 0 };
	struct fib_nh_common *nhc;
	int ifindex;

	if (nh_grp->nhgi->count != nhs)
		return false;

	for (u8 i = 0; i < nhs; i++) {
		nhc = sparx5_rr_fib_info_nhc(fi, i);
		ifindex = nhc->nhc_dev ? nhc->nhc_dev->ifindex : -1;
		sparx5_rr_fib_nhc_to_gw(nhc, fi->version, &gw);

		if (!sparx5_rr_nexthop_group_has_nexthop(nh_grp, &gw, ifindex))
			return false;
	}

	return true;
}

static u16 sparx5_rr_route_sort_key(u32 prefix_len)
{
	/* Order longer prefixes at high addresses. */
	return SPARX5_IADDR_LEN(SPARX5_IPV6) - prefix_len;
}

static void sparx5_rr_to_fib4_key(u32 dst, int dst_len, u32 tb_id,
				  struct sparx5_rr_fib_key *key)
{
	memset(key, 0, sizeof(*key));
	key->addr.version = SPARX5_IPV4;
	key->addr.ipv4 = cpu_to_be32(dst);
	key->prefix_len = dst_len;
	key->tb_id = tb_id;
}

static void sparx5_rr_to_fib6_key(struct in6_addr *addr, int prefix_len,
				  u32 tb_id, struct sparx5_rr_fib_key *key)
{
	memset(key, 0, sizeof(*key));
	key->addr.version = SPARX5_IPV6;
	memcpy(&key->addr.ipv6, addr, sizeof(*addr));
	key->prefix_len = prefix_len;
	key->tb_id = tb_id;
}

static void sparx5_rr_fib_info_to_fib_key(struct sparx5_rr_fib_info *fi,
					  struct sparx5_rr_fib_key *key)
{
	struct fib_entry_notifier_info *fen_info;
	struct fib6_info *rt;

	switch (fi->version) {
	case SPARX5_IPV4:
		fen_info = &fi->fen4_info;
		sparx5_rr_to_fib4_key(fen_info->dst, fen_info->dst_len,
				      fen_info->tb_id, key);
		return;
	case SPARX5_IPV6:
		rt = fi->fe6_info.rt_arr[0];

		sparx5_rr_to_fib6_key(&rt->fib6_dst.addr, rt->fib6_dst.plen,
				      rt->fib6_table->tb6_id, key);
		return;
	}
}

static bool
sparx5_rr_fib_entry_lpm4_match(__be32 addr,
			       struct sparx5_rr_fib_entry *fib_entry)
{
	__be32 mask = inet_make_mask(fib_entry->key.prefix_len);

	return !((addr ^ fib_entry->key.addr.ipv4) & mask);
}

static void sparx5_rr_inet6_make_mask(int logmask, struct in6_addr *mask)
{
	/* Caller must ensure 0 <= logmask <= 128 */
	int rem, byte_prefix = logmask;

	rem = do_div(byte_prefix, BITS_PER_BYTE);

	memset(mask, 0, sizeof(*mask));
	memset(mask, 0xff, byte_prefix);

	if (rem)
		mask->in6_u.u6_addr8[byte_prefix] = GENMASK(7, 7 - rem + 1);
}

static void sparx5_rr_inet6_make_mask_le(int logmask, u8 *mask)
{
	/* Caller must ensure 0 <= logmask <= 128 */
	int rem, byte_prefix = logmask;

	rem = do_div(byte_prefix, BITS_PER_BYTE);

	memset(mask, 0, 16);

	for (int i = 0; i < byte_prefix; i++)
		mask[15 - i] = 0xff;

	if (rem)
		mask[15 - byte_prefix] = GENMASK(7, 7 - rem + 1);
}

static bool
sparx5_rr_fib_entry_lpm6_match(struct in6_addr *addr,
			       struct sparx5_rr_fib_entry *fib_entry)
{
	struct in6_addr mask = { 0 };

	sparx5_rr_inet6_make_mask(fib_entry->key.prefix_len, &mask);

	return !ipv6_masked_addr_cmp(addr, &mask, &fib_entry->key.addr.ipv6);
}

static bool sparx5_rr_fib_entry_lpm_match(struct sparx5_iaddr *addr,
					  struct sparx5_rr_fib_entry *fib_entry)
{
	switch (addr->version) {
	case SPARX5_IPV4:
		return sparx5_rr_fib_entry_lpm4_match(addr->ipv4, fib_entry);
	case SPARX5_IPV6:
		return sparx5_rr_fib_entry_lpm6_match(&addr->ipv6, fib_entry);
	default:
		WARN_ON(1);
		return false;
	}
}

static struct list_head *sparx5_rr_fib_lpm_get(struct sparx5 *sparx5,
					       struct sparx5_iaddr *addr)
{
	switch (addr->version) {
	case SPARX5_IPV4:
		return &sparx5->router->fib_lpm4_list;
	case SPARX5_IPV6:
		return &sparx5->router->fib_lpm6_list;
	default:
		WARN_ON(1);
		return NULL;
	}
}

static struct sparx5_rr_fib_entry *
sparx5_rr_fib_lpm_lookup(struct sparx5 *sparx5, struct sparx5_iaddr *addr)
{
	struct list_head *lpm_backend = sparx5_rr_fib_lpm_get(sparx5, addr);
	struct sparx5_rr_fib_entry *iter;

	list_for_each_entry(iter, lpm_backend, fib_lpm_node)
		if (sparx5_rr_fib_entry_lpm_match(addr, iter))
			return iter;

	return NULL;
}

static bool
sparx5_rr_fib_lpm_is_interesting(struct sparx5_rr_fib_entry *fib_entry)
{
	/* No need to search through local FIB entries */
	return fib_entry->type == SPARX5_RR_FIB_TYPE_UNICAST;
}

static void sparx5_rr_fib_lpm_insert(struct sparx5 *sparx5,
				     struct sparx5_rr_fib_entry *fib_entry)
{
	struct list_head *lpm_backend =
		sparx5_rr_fib_lpm_get(sparx5, &fib_entry->key.addr);
	struct sparx5_rr_fib_entry *iter, *next = NULL;

	if (!sparx5_rr_fib_lpm_is_interesting(fib_entry))
		return;

	list_for_each_entry(iter, lpm_backend, fib_lpm_node) {
		if (fib_entry->sort_key < iter->sort_key) {
			next = iter;
			break;
		}
	}

	if (!next) {
		list_add_tail(&fib_entry->fib_lpm_node, lpm_backend);
		return;
	}

	/* Add before next entry */
	list_add_tail(&fib_entry->fib_lpm_node, &next->fib_lpm_node);
}

static void sparx5_rr_fib_lpm_remove(struct sparx5_rr_fib_entry *fib_entry)
{
	if (!sparx5_rr_fib_lpm_is_interesting(fib_entry))
		return;

	list_del(&fib_entry->fib_lpm_node);
}

static int sparx5_rr_lpm_rule6_xip_add(struct vcap_rule *rule,
				       struct sparx5_iaddr *addr,
				       u32 prefix_len)
{
	struct vcap_u128_key addr_key;

	/* HW value/mask must be little endian */
	sparx5_rr_inet6_make_mask_le(prefix_len, addr_key.mask);

	for (int i = 0; i < 16; i++)
		addr_key.value[i] = addr->ipv6.s6_addr[15 - i];

	return vcap_rule_add_key_u128(rule, VCAP_KF_IP6_XIP, &addr_key);
}

static int sparx5_rr_lpm_rule4_xip_add(struct vcap_rule *rule,
				       struct sparx5_iaddr *addr,
				       u32 prefix_len)
{
	u32 mask = ntohl(inet_make_mask(prefix_len));
	u32 iaddr = ntohl(addr->ipv4);

	return vcap_rule_add_key_u32(rule, VCAP_KF_IP4_XIP, iaddr, mask);
}

static int sparx5_rr_lpm_rule_xip_add(struct vcap_rule *rule,
				      struct sparx5_iaddr *addr, u32 prefix_len)
{
	switch (addr->version) {
	case SPARX5_IPV4:
		return sparx5_rr_lpm_rule4_xip_add(rule, addr, prefix_len);
	case SPARX5_IPV6:
		return sparx5_rr_lpm_rule6_xip_add(rule, addr, prefix_len);
	default:
		WARN_ON(1);
		return -EINVAL;
	}
}

static struct sparx5_rr_router_leg *
sparx5_rr_leg_find_by_dev(struct sparx5 *sparx5, struct net_device *dev)
{
	struct sparx5_rr_router_leg *leg;

	list_for_each_entry(leg, &sparx5->router->leg_list, leg_list_node) {
		if (leg->dev == dev)
			return leg;
	}

	return NULL;
}

static struct sparx5_rr_fib_entry *
sparx5_rr_fib_entry_lookup(struct sparx5 *sparx5, struct sparx5_rr_fib_key *key)
{
	return rhashtable_lookup_fast(&sparx5->router->fib_ht, key,
				      sparx5_rr_fib_entry_ht_params);
}

static int sparx5_rr_fib_entry_insert(struct sparx5 *sparx5,
				      struct sparx5_rr_fib_entry *fib_entry)
{
	return rhashtable_insert_fast(&sparx5->router->fib_ht,
				      &fib_entry->ht_node,
				      sparx5_rr_fib_entry_ht_params);
}

static void sparx5_rr_fib_entry_remove(struct sparx5 *sparx5,
				       struct sparx5_rr_fib_entry *fib_entry)
{
	rhashtable_remove_fast(&sparx5->router->fib_ht, &fib_entry->ht_node,
			       sparx5_rr_fib_entry_ht_params);
}

static struct sparx5_rr_neigh_entry *
sparx5_rr_neigh_entry_lookup(struct sparx5 *sparx5,
			     struct sparx5_rr_neigh_key *key)
{
	return rhashtable_lookup_fast(&sparx5->router->neigh_ht, key,
				      sparx5_neigh_ht_params);
}

static int sparx5_rr_neigh_entry_insert(struct sparx5 *sparx5,
					struct sparx5_rr_neigh_entry *entry)
{
	return rhashtable_insert_fast(&sparx5->router->neigh_ht,
				      &entry->ht_node, sparx5_neigh_ht_params);
}

static void sparx5_rr_neigh_entry_remove(struct sparx5 *sparx5,
					 struct sparx5_rr_neigh_entry *entry)
{
	rhashtable_remove_fast(&sparx5->router->neigh_ht, &entry->ht_node,
			       sparx5_neigh_ht_params);
}

static int sparx5_lower_dev_walk(struct net_device *lower_dev,
				 struct netdev_nested_priv *priv)
{
	int ret = 0;

	if (sparx5_netdevice_check(lower_dev)) {
		priv->data = (void *)netdev_priv(lower_dev);
		ret = 1;
	}

	return ret;
}

static struct sparx5_port *sparx5_port_dev_lower_find_rcu(struct net_device *dev)
{
	struct netdev_nested_priv priv = {
		.data = NULL,
	};

	if (sparx5_netdevice_check(dev))
		return netdev_priv(dev);

	netdev_walk_all_lower_dev_rcu(dev, sparx5_lower_dev_walk, &priv);

	return priv.data;
}

static struct sparx5_port *sparx5_port_dev_lower_find(struct net_device *dev)
{
	struct sparx5_port *port;

	rcu_read_lock();
	port = sparx5_port_dev_lower_find_rcu(dev);
	rcu_read_unlock();

	return port;
}

static struct sparx5_rr_neigh_entry *
sparx5_rr_neigh_entry_alloc(struct sparx5 *sparx5,
			    struct sparx5_rr_neigh_key *key,
			    struct sparx5_rr_router_leg *leg)
{
	struct sparx5_rr_neigh_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return NULL;

	memcpy(&entry->key, key, sizeof(*key));

	entry->vmid = leg->vmid;
	entry->hw_route.vrule_id = 0;
	entry->hw_route.vrule_id_valid = false;
	entry->fib_entry = NULL;
	entry->neigh_tbl = NULL;

	switch (key->iaddr.version) {
	case SPARX5_IPV4:
		entry->neigh_tbl = &arp_tbl;
		break;
	case SPARX5_IPV6:
#if IS_ENABLED(CONFIG_IPV6)
		entry->neigh_tbl = &nd_tbl;
		break;
#else
		return NULL;
#endif
	}

	eth_zero_addr(entry->hwaddr);

	INIT_LIST_HEAD(&entry->nexthop_list);
	INIT_LIST_HEAD(&entry->fib_list_node);

	return entry;
}

static bool sparx5_rr_addr_is_link_local(struct sparx5_iaddr *iaddr)
{
	if (iaddr->version != SPARX5_IPV6)
		return false;

	return ipv6_addr_type(&iaddr->ipv6) & IPV6_ADDR_LINKLOCAL;
}

static int sparx5_rr_neigh_entry_fib_link(struct sparx5 *sparx5,
					  struct sparx5_rr_neigh_entry *entry)
{
	struct sparx5_rr_fib_entry *fib_entry;

	fib_entry = sparx5_rr_fib_lpm_lookup(sparx5, &entry->key.iaddr);
	if (!fib_entry)
		return -ENOENT;

	list_add(&entry->fib_list_node, &fib_entry->neigh_list);
	entry->fib_entry = fib_entry;

	sparx5_rr_fib_link_debug(sparx5, entry, fib_entry);

	return 0;
}

static struct sparx5_rr_neigh_entry *
sparx5_rr_neigh_entry_create(struct sparx5 *sparx5,
			     struct sparx5_rr_neigh_key *key)
{
	struct sparx5_rr_neigh_entry *entry;
	struct sparx5_rr_router_leg *leg;
	struct sparx5_port *port_below;
	int err;

	port_below = sparx5_port_dev_lower_find(key->dev);
	if (!port_below)
		return ERR_PTR(-EINVAL);

	leg = sparx5_rr_leg_find_by_dev(sparx5, key->dev);
	if (!leg)
		return ERR_PTR(-EINVAL);

	entry = sparx5_rr_neigh_entry_alloc(sparx5, key, leg);
	if (!entry)
		return ERR_PTR(-ENOMEM);

	err = sparx5_rr_neigh_entry_insert(sparx5, entry);
	if (err)
		goto err_insert;

	/* Link neigh to the fib which owns the subnet. */
	err = sparx5_rr_neigh_entry_fib_link(sparx5, entry);
	if (err)
		goto err_fib_link;

	netdev_hold(entry->key.dev, NULL, GFP_KERNEL);

	sparx5_rr_neigh_debug(sparx5, entry, "create");

	return entry;

err_fib_link:
	sparx5_rr_neigh_entry_remove(sparx5, entry);
err_insert:
	kfree(entry);
	return ERR_PTR(err);
}

static int sparx5_rr_nexthop_neigh_init(struct sparx5 *sparx5,
					struct sparx5_rr_router_leg *leg,
					struct sparx5_rr_nexthop *nh)
{
	struct sparx5_rr_neigh_entry *neigh_entry;
	struct net_device *dev = leg->dev;
	struct sparx5_rr_neigh_key key;
	struct neighbour *n;
	int err = 0;

	if (!nh->gateway || nh->neigh_entry || !nh->neigh_tbl)
		return 0;

	/* Look up neighbor in the global neighbor table. Takes ref to n. */
	n = neigh_lookup(nh->neigh_tbl, &nh->gw_addr, dev);
	if (!n) {
		n = neigh_create(nh->neigh_tbl, &nh->gw_addr, dev);
		if (IS_ERR(n))
			return PTR_ERR(n);
		/* Start arp process */
		neigh_event_send(n, NULL);
	}

	sparx5_rr_nb2neigh_key(n, &key);

	neigh_entry = sparx5_rr_neigh_entry_lookup(sparx5, &key);
	if (!neigh_entry) {
		neigh_entry = sparx5_rr_neigh_entry_create(sparx5, &key);
		if (IS_ERR(neigh_entry)) {
			err = PTR_ERR(neigh_entry);
			goto out;
		}
	}

	nh->neigh_entry = neigh_entry;
	list_add_tail(&nh->neigh_list_node, &neigh_entry->nexthop_list);

out:
	neigh_release(n);
	return err;
}

static void sparx5_rr_neigh_entry_destroy(struct sparx5 *sparx5,
					  struct sparx5_rr_neigh_entry *entry)
{
	WARN_ON(entry->hw_route.vrule_id_valid);

	if (entry->fib_entry)
		list_del(&entry->fib_list_node);

	sparx5_rr_neigh_entry_offload_mark(entry, false);
	sparx5_rr_neigh_entry_remove(sparx5, entry);
	netdev_put(entry->key.dev, NULL);

	sparx5_rr_neigh_debug(sparx5, entry, "destroy");

	kfree(entry);
}

static void sparx5_rr_neigh_entry_put(struct sparx5 *sparx5,
				      struct sparx5_rr_neigh_entry *neigh_entry)
{
	if (neigh_entry && list_empty(&neigh_entry->nexthop_list) &&
	    !neigh_entry->hw_route.vrule_id_valid)
		sparx5_rr_neigh_entry_destroy(sparx5, neigh_entry);
}

static void sparx5_rr_nexthop_deinit(struct sparx5 *sparx5,
				     struct sparx5_rr_nexthop *nh)
{
	struct sparx5_rr_neigh_entry *neigh_entry = nh->neigh_entry;

	if (neigh_entry) {
		list_del(&nh->neigh_list_node);
		sparx5_rr_neigh_entry_put(sparx5, neigh_entry);
	}

	nh->neigh_entry = NULL;
}

static int sparx5_rr_nexthop_init(struct sparx5 *sparx5,
				  struct sparx5_rr_nexthop_group *nh_grp,
				  struct sparx5_rr_nexthop *nh,
				  struct fib_nh_common *fnhc)
{
	struct sparx5_rr_router_leg *leg;

	nh->ifindex = -1;
	nh->grp = nh_grp;
	nh->gateway = fnhc->nhc_gw_family != 0;
	nh->trapped = false;
	nh->neigh_entry = NULL;
	nh->neigh_tbl = NULL;

	memset(&nh->gw_addr, 0, sizeof(nh->gw_addr));

	if (!nh->gateway)
		return 0;

	switch (fnhc->nhc_gw_family) {
	case AF_INET:
		nh->gw_addr.version = SPARX5_IPV4;
		nh->gw_addr.ipv4 = fnhc->nhc_gw.ipv4;
		nh->neigh_tbl = &arp_tbl;
		break;
	case AF_INET6:
		nh->gw_addr.version = SPARX5_IPV6;
		nh->gw_addr.ipv6 = fnhc->nhc_gw.ipv6;
#if IS_ENABLED(CONFIG_IPV6)
		nh->neigh_tbl = &nd_tbl;
		break;
#else
		return -EINVAL;
#endif
	default:
		WARN_ON_ONCE(1); /* BUG */
		return 0;
	}

	/* Blackhole route nexthops have no egress device. */
	if (!fnhc->nhc_dev)
		return 0;

	nh->ifindex = fnhc->nhc_dev->ifindex;

	/* When a router leg is removed, all the nexthops with gateway IPs in a
	 * subnet governed by the leg will receive fib delete events. However,
	 * these delete events are received one by one. Therefore, this nexthop
	 * init could have been triggered by a group resize action for such an
	 * event, where the underlying leg is already removed.
	 *
	 * This is not an error. We handle this during offloading by
	 * trapping nexthops which do not have a neigh_entry. As fib deletion
	 * events are processed, we converge to the proper state.
	 */
	leg = sparx5_rr_leg_find_by_dev(sparx5, fnhc->nhc_dev);
	if (!leg)
		return 0;

	return sparx5_rr_nexthop_neigh_init(sparx5, leg, nh);
}

static int
sparx5_rr_nexthop_group_info_init(struct sparx5 *sparx5,
				  struct sparx5_rr_nexthop_group *nh_grp,
				  struct sparx5_rr_fib_info *fi)
{
	unsigned int nhs = sparx5_rr_fib_info_nhs(fi);
	struct sparx5_rr_nexthop_group_info *nhgi;
	struct sparx5_rr_nexthop *nh;
	int err, i;

	nhgi = kzalloc(struct_size(nhgi, nexthops, nhs), GFP_KERNEL);
	if (!nhgi)
		return -ENOMEM;

	nh_grp->nhgi = nhgi;
	nhgi->grp = nh_grp;
	nhgi->atbl_offset_valid = false;
	nhgi->atbl_offset = 0;
	nhgi->count = nhs;

	for (i = 0; i < nhgi->count; i++) {
		struct fib_nh_common *fnhc;

		nh = &nhgi->nexthops[i];
		fnhc = sparx5_rr_fib_info_nhc(fi, i);
		err = sparx5_rr_nexthop_init(sparx5, nh_grp, nh, fnhc);
		if (err)
			goto err_nexthop_init;
	}

	return 0;

err_nexthop_init:
	for (i--; i >= 0; i--) {
		nh = &nhgi->nexthops[i];
		sparx5_rr_nexthop_deinit(sparx5, nh);
	}
	kfree(nhgi);
	return err;
}

static void
sparx5_rr_nexthop_group_info_deinit(struct sparx5 *sparx5,
				    struct sparx5_rr_nexthop_group *nh_grp)
{
	struct sparx5_rr_nexthop_group_info *nhgi = nh_grp->nhgi;
	struct sparx5_rr_nexthop *nh;
	int i;

	WARN_ON(!nhgi->count);
	WARN_ON_ONCE(nhgi->atbl_offset_valid);

	for (i = nhgi->count - 1; i >= 0; i--) {
		nh = &nhgi->nexthops[i];

		sparx5_rr_nexthop_deinit(sparx5, nh);
	}

	kfree(nhgi);
}

static void sparx5_rr_arp_tbl_hw_addr_apply(struct sparx5 *sparx5,
					    unsigned char mac[ETH_ALEN],
					    u16 evmid, int offset)
{
	u32 mac_msb, mac_lsb;

	sparx5_rr_split_mac(mac, 32, &mac_msb, &mac_lsb);

	spx5_rmw(ANA_L3_ARP_CFG_0_MAC_MSB_SET(mac_msb) |
		 ANA_L3_ARP_CFG_0_ARP_VMID_SET(evmid) |
		 ANA_L3_ARP_CFG_0_ARP_ENA_SET(1),
		 ANA_L3_ARP_CFG_0_ARP_ENA |
		 ANA_L3_ARP_CFG_0_ARP_VMID |
		 ANA_L3_ARP_CFG_0_MAC_MSB,
		 sparx5, ANA_L3_ARP_CFG_0(offset));

	spx5_wr(mac_lsb, sparx5, ANA_L3_ARP_CFG_1(offset));
}

static void sparx5_rr_arp_tbl_hw_addr_clear(struct sparx5 *sparx5, int offset)
{
	spx5_rmw(ANA_L3_ARP_CFG_0_ARP_ENA_SET(0), ANA_L3_ARP_CFG_0_ARP_ENA,
		 sparx5, ANA_L3_ARP_CFG_0(offset));
}

static void
sparx5_rr_nh_grp_arp_tbl_grp_clear(struct sparx5 *sparx5,
				   struct sparx5_rr_nexthop_group *nh_grp)
{
	int offset = nh_grp->nhgi->atbl_offset;

	if (nh_grp->nhgi->atbl_offset_valid)
		for (u8 i = 0; i < nh_grp->nhgi->count; i++)
			sparx5_rr_arp_tbl_hw_addr_clear(sparx5, offset + i);

	sparx5_rr_arp_tbl_grp_free(sparx5, nh_grp->nhgi->count, offset);
	nh_grp->nhgi->atbl_offset_valid = false;
}

static void sparx5_rr_nexthop_group_put(struct sparx5 *sparx5,
					struct sparx5_rr_nexthop_group *nh_grp)
{
	sparx5_rr_nh_grp_arp_tbl_grp_clear(sparx5, nh_grp);
	sparx5_rr_nexthop_group_info_deinit(sparx5, nh_grp);
	kfree(nh_grp);
}

static struct sparx5_rr_nexthop_group *
sparx5_rr_nexthop_group_create(struct sparx5 *sparx5,
			       struct sparx5_rr_fib_entry *fib_entry)
{
	struct sparx5_rr_nexthop_group *nh_grp;
	int err;

	nh_grp = kzalloc(sizeof(*nh_grp), GFP_KERNEL);
	if (!nh_grp)
		return ERR_PTR(-ENOMEM);

	err = sparx5_rr_nexthop_group_info_init(sparx5, nh_grp, &fib_entry->fi);
	if (err)
		goto err_group_info_init;

	return nh_grp;

err_group_info_init:
	kfree(nh_grp);
	return ERR_PTR(err);
}

static enum sparx5_rr_fib_type sparx5_rr_rtm_type2fib_type(u8 type)
{
	switch (type) {
	case RTN_UNICAST:
		return SPARX5_RR_FIB_TYPE_UNICAST;
	case RTN_LOCAL:
		return SPARX5_RR_FIB_TYPE_LOCAL;
	case RTN_MULTICAST:
		return SPARX5_RR_FIB_TYPE_MULTICAST;
	case RTN_BLACKHOLE:
		return SPARX5_RR_FIB_TYPE_BLACKHOLE;
	case RTN_PROHIBIT:
		return SPARX5_RR_FIB_TYPE_PROHIBIT;
	case RTN_UNREACHABLE:
		return SPARX5_RR_FIB_TYPE_UNREACHABLE;
	default:
		return SPARX5_RR_FIB_TYPE_INVALID;
	}
}

static void
sparx5_rr_fib_entry_fib4_info_set(struct sparx5_rr_fib_entry *fib_entry,
				  struct fib_entry_notifier_info *fen4_info)
{
	/* Prevent the fib_info from being deleted while we store the fen_info */
	fib_info_hold(fen4_info->fi);
	memcpy(&fib_entry->fi.fen4_info, fen4_info, sizeof(*fen4_info));
}

static int
sparx5_rr_fib_entry_fib6_info_add(struct sparx5_rr_fib_entry *fib_entry,
				  struct sparx5_rr_fib6_entry_info *fib6_info)
{
	struct sparx5_rr_fib6_entry_info *f6i = &fib_entry->fi.fe6_info;
	unsigned int old_ntr6 = f6i->nrt6;
	unsigned int new_ntr6 = old_ntr6 + fib6_info->nrt6;
	struct fib6_info **rt_arr;

	rt_arr = kcalloc(new_ntr6, sizeof(struct fib6_info *), GFP_KERNEL);
	if (!rt_arr)
		return -ENOMEM;

	/* Copy existing */
	for (int i = 0; i < old_ntr6; i++)
		rt_arr[i] = f6i->rt_arr[i];

	/* Copy new and hold fib6_info */
	for (int i = 0; i < fib6_info->nrt6; i++) {
		struct fib6_info *rt = fib6_info->rt_arr[i];

		rt_arr[old_ntr6 + i] = rt;
		fib6_info_hold(rt);
	}

	/* Free old fib6_info */
	kfree(f6i->rt_arr);
	f6i->rt_arr = rt_arr;
	f6i->nrt6 = new_ntr6;

	WARN_ON(!fib_entry->fi.fe6_info.rt_arr);
	WARN_ON(!fib_entry->fi.fe6_info.nrt6);

	return 0;
}

static int
sparx5_rr_fib_entry_fib_info_add(struct sparx5_rr_fib_entry *fib_entry,
				 struct sparx5_rr_fib_info *fi)
{
	switch (fi->version) {
	case SPARX5_IPV4:
		/* IPv4 nexthops can not be added/removed piecemeal similar to
		 * IPv6, so this is a replace in practice.
		 */
		sparx5_rr_fib_entry_fib4_info_set(fib_entry, &fi->fen4_info);
		return 0;
	case SPARX5_IPV6:
		return sparx5_rr_fib_entry_fib6_info_add(fib_entry,
							 &fi->fe6_info);
	default:
		WARN_ON(1);
		return 0;
	}
}

static struct sparx5_rr_fib_entry *
sparx5_rr_fib_entry_create(struct sparx5 *sparx5, struct sparx5_rr_fib_key *key,
			   struct sparx5_rr_fib_info *fi)
{
	struct sparx5_rr_nexthop_group *nh_grp;
	u8 type = sparx5_rr_fib_info_type(fi);
	struct sparx5_rr_fib_entry *fib_entry;
	int err;

	fib_entry = kzalloc(sizeof(*fib_entry), GFP_KERNEL);
	if (!fib_entry)
		return ERR_PTR(-ENOMEM);

	memcpy(&fib_entry->key, key, sizeof(*key));
	sparx5_rr_fib_info_init(&fib_entry->fi, fi->version);
	fib_entry->type = sparx5_rr_rtm_type2fib_type(type);
	fib_entry->sort_key = sparx5_rr_route_sort_key(key->prefix_len);
	fib_entry->hw_route.vrule_id = 0;
	fib_entry->hw_route.vrule_id_valid = false;

	err = sparx5_rr_fib_entry_fib_info_add(fib_entry, fi);
	if (err)
		goto err_fib_info_set;

	err = sparx5_rr_fib_entry_insert(sparx5, fib_entry);
	if (err)
		goto err_fib_entry_insert;

	nh_grp = sparx5_rr_nexthop_group_create(sparx5, fib_entry);
	if (IS_ERR(nh_grp)) {
		err = PTR_ERR(nh_grp);
		goto err_nexthop_group_create;
	}

	fib_entry->nh_grp = nh_grp;
	nh_grp->fib_entry = fib_entry;
	INIT_LIST_HEAD(&fib_entry->neigh_list);

	sparx5_rr_fib_lpm_insert(sparx5, fib_entry);

	sparx5_rr_fib_debug(sparx5, fib_entry, "create");

	return fib_entry;

err_nexthop_group_create:
	sparx5_rr_fib_entry_remove(sparx5, fib_entry);
err_fib_entry_insert:
	sparx5_rr_fib_info_put(&fib_entry->fi);
err_fib_info_set:
	kfree(fib_entry);

	return ERR_PTR(err);
}

#if IS_ENABLED(CONFIG_IPV6)
static void
sparx5_rr_fib6_entry_offload_mark(struct sparx5 *sparx5,
				  struct sparx5_rr_fib6_entry_info *fen6_info,
				  bool offload,
				  bool trap,
				  bool offload_failed)
{
	for (int i = 0; i < fen6_info->nrt6; i++)
		fib6_info_hw_flags_set(&init_net, fen6_info->rt_arr[i], offload,
				       trap, offload_failed);
}
#else
static void
sparx5_rr_fib6_entry_offload_mark(struct sparx5 *sparx5,
				  struct sparx5_rr_fib6_entry_info *fen6_info,
				  bool offload,
				  bool trap,
				  bool offload_failed)
{
}
#endif

static void
sparx5_rr_fib4_entry_offload_mark(struct sparx5 *sparx5,
				  struct fib_entry_notifier_info *fen4_info,
				  bool offload,
				  bool trap,
				  bool offload_failed)
{
	struct fib_rt_info fri;

	fri.fi = fen4_info->fi;
	fri.tb_id = fen4_info->tb_id;
	fri.dst = cpu_to_be32(fen4_info->dst);
	fri.dst_len = fen4_info->dst_len;
	fri.dscp = fen4_info->dscp;
	fri.type = fen4_info->type;
	fri.offload = offload;
	fri.trap = trap;
	fri.offload_failed = offload_failed;

	fib_alias_hw_flags_set(&init_net, &fri);
}

static void sparx5_rr_fib_info_offload_mark(struct sparx5 *sparx5,
					    struct sparx5_rr_fib_info *fi,
					    bool offload, bool trap,
					    bool offload_failed)
{
	switch (fi->version) {
	case SPARX5_IPV4:
		return sparx5_rr_fib4_entry_offload_mark(sparx5,
							 &fi->fen4_info,
							 offload, trap,
							 offload_failed);
	case SPARX5_IPV6:
		return sparx5_rr_fib6_entry_offload_mark(sparx5,
							 &fi->fe6_info,
							 offload, trap,
							 offload_failed);
	}
}

static void
sparx5_rr_fib_entry_offload_mark(struct sparx5 *sparx5,
				 struct sparx5_rr_fib_entry *fib_entry)
{
	bool offload, trap, offload_failed;

	offload_failed = fib_entry->offload_fail;
	offload = !fib_entry->offload_fail;
	trap = !fib_entry->offload_fail && fib_entry->trap;

	sparx5_rr_fib_info_offload_mark(sparx5, &fib_entry->fi, offload, trap,
					offload_failed);
}

static int
sparx5_rr_lpm_arp_entry_create(struct sparx5 *sparx5,
			       struct sparx5_iaddr *addr,
			       u32 prefix_len, unsigned char mac[ETH_ALEN],
			       u16 evmid, struct sparx5_rr_hw_route *hw_route)
{
	u32 priority = sparx5_rr_route_sort_key(prefix_len);
	struct net_device *pdev = sparx5->router->port_dev;
	struct vcap_control *vctrl = sparx5->vcap_ctrl;
	struct vcap_rule *rule;
	u32 mac_msb, mac_lsb;
	int err;

	sparx5_rr_split_mac(mac, 32, &mac_msb, &mac_lsb);

	rule = vcap_alloc_rule(vctrl, pdev, VCAP_CID_PREROUTING_L0,
			       VCAP_USER_L3, priority, 0);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	err = sparx5_rr_lpm_rule_xip_add(rule, addr, prefix_len);
	if (err)
		goto free_rule;

	err = vcap_rule_add_key_u32(rule, VCAP_KF_AFFIX, 0, 0);
	if (err)
		goto free_rule;

	err = vcap_rule_add_key_bit(rule, VCAP_KF_DST_FLAG, VCAP_BIT_1);
	if (err)
		goto free_rule;

	err = vcap_rule_add_action_u32(rule, VCAP_AF_MAC_MSB, mac_msb);
	if (err)
		goto free_rule;

	err = vcap_rule_add_action_u32(rule, VCAP_AF_MAC_LSB, mac_lsb);
	if (err)
		goto free_rule;

	err = vcap_rule_add_action_u32(rule, VCAP_AF_ARP_VMID, evmid);
	if (err)
		goto free_rule;

	err = vcap_rule_add_action_bit(rule, VCAP_AF_ARP_ENA, VCAP_BIT_1);
	if (err)
		goto free_rule;

	err = vcap_val_rule(rule, sparx5_rr_iaddr_proto(addr));
	if (err)
		goto free_rule;

	hw_route->vrule_id = rule->id;
	hw_route->vrule_id_valid = true;
	err = vcap_add_rule(rule);

free_rule:
	vcap_free_rule(rule);

	return err;
}

static int sparx5_rr_lpm_arp_entry_mod(struct sparx5 *sparx5,
				       unsigned char mac[ETH_ALEN], u16 evmid,
				       u32 vrule_id)
{
	struct vcap_control *vctrl = sparx5->vcap_ctrl;
	struct vcap_rule *vrule;
	u32 mac_msb, mac_lsb;
	int err;

	sparx5_rr_split_mac(mac, 32, &mac_msb, &mac_lsb);

	vrule = vcap_get_rule(vctrl, vrule_id);
	if (IS_ERR(vrule))
		return -EINVAL;

	err = vcap_rule_mod_action_u32(vrule, VCAP_AF_MAC_MSB, mac_msb);
	if (err)
		goto free_rule;

	err = vcap_rule_mod_action_u32(vrule, VCAP_AF_MAC_LSB, mac_lsb);
	if (err)
		goto free_rule;

	err = vcap_rule_mod_action_u32(vrule, VCAP_AF_ARP_VMID, evmid);
	if (err)
		goto free_rule;

	err = vcap_rule_mod_action_bit(vrule, VCAP_AF_ARP_ENA, VCAP_BIT_1);
	if (err)
		goto free_rule;

	err = vcap_mod_rule(vrule);

free_rule:
	vcap_free_rule(vrule);

	return err;
}

static int
sparx5_rr_fib_entry_update_arp_entry(struct sparx5 *sparx5,
				     struct sparx5_rr_fib_entry *fib_entry,
				     unsigned char mac[ETH_ALEN], u16 evmid)
{
	struct net_device *pdev = sparx5->router->port_dev;
	struct vcap_control *vctrl = sparx5->vcap_ctrl;
	u32 vrule_id = fib_entry->hw_route.vrule_id;
	struct vcap_rule *vrule;
	u32 mac_msb, mac_lsb;
	int err;

	sparx5_rr_split_mac(mac, 32, &mac_msb, &mac_lsb);

	vrule = vcap_get_rule(vctrl, vrule_id);
	if (IS_ERR(vrule)) {
		fib_entry->hw_route.vrule_id_valid = false;
		return PTR_ERR(vrule);
	}

	switch (vrule->actionset) {
	case VCAP_AFS_ARP_ENTRY:
		err = vcap_rule_mod_action_u32(vrule, VCAP_AF_MAC_MSB, mac_msb);
		if (err)
			goto free_rule;

		err = vcap_rule_mod_action_u32(vrule, VCAP_AF_MAC_LSB, mac_lsb);
		if (err)
			goto free_rule;

		err = vcap_mod_rule(vrule);
		goto free_rule;
	case VCAP_AFS_ARP_PTR:
		/* Convert arp_ptr to arp_entry */
		err = sparx5_rr_lpm_arp_entry_create(sparx5,
						     &fib_entry->key.addr,
						     fib_entry->key.prefix_len,
						     mac, evmid,
						     &fib_entry->hw_route);
		if (err)
			goto free_rule;

		sparx5_rr_nh_grp_arp_tbl_grp_clear(sparx5, fib_entry->nh_grp);
		err = vcap_del_rule(vctrl, pdev, vrule_id);
		goto free_rule;
	default:
		err = -EINVAL;
		WARN_ON(1); /* BUG */
	}

free_rule:
	vcap_free_rule(vrule);

	return err;
}

static int sparx5_rr_lpm_arp_ptr_create(struct sparx5 *sparx5,
					struct sparx5_iaddr *addr,
					u32 prefix_len, u32 arp_offset_addr,
					u8 ecmp_size,
					struct sparx5_rr_hw_route *hw_route)
{
	u32 priority = sparx5_rr_route_sort_key(prefix_len);
	struct net_device *pdev = sparx5->router->port_dev;
	struct vcap_control *vctrl = sparx5->vcap_ctrl;
	struct vcap_rule *rule;
	int err;

	rule = vcap_alloc_rule(vctrl, pdev, VCAP_CID_PREROUTING_L0,
			       VCAP_USER_L3, priority, 0);
	if (IS_ERR(rule))
		return PTR_ERR(rule);

	err = sparx5_rr_lpm_rule_xip_add(rule, addr, prefix_len);
	if (err)
		goto out;

	err = vcap_rule_add_key_u32(rule, VCAP_KF_AFFIX, 0, 0);
	if (err)
		goto out;

	err = vcap_rule_add_key_bit(rule, VCAP_KF_DST_FLAG, VCAP_BIT_1);
	if (err)
		goto out;

	err = vcap_rule_add_action_u32(rule, VCAP_AF_ARP_PTR, arp_offset_addr);
	if (err)
		goto out;

	err = vcap_rule_add_action_bit(rule, VCAP_AF_ARP_PTR_REMAP_ENA,
				       VCAP_BIT_0);
	if (err)
		goto out;

	err = vcap_rule_add_action_u32(rule, VCAP_AF_ECMP_CNT, ecmp_size - 1);
	if (err)
		goto out;

	err = vcap_rule_add_action_u32(rule, VCAP_AF_RGID, 0);
	if (err)
		goto out;

	err = vcap_val_rule(rule, sparx5_rr_iaddr_proto(addr));
	if (err)
		goto out;

	hw_route->vrule_id = rule->id;
	hw_route->vrule_id_valid = true;
	err = vcap_add_rule(rule);

out:
	vcap_free_rule(rule);
	return err;
}

/* Get egress mac and vmid for nexthop. */
static void sparx5_rr_nexthop_egress_derive(struct sparx5_rr_nexthop *nh,
					    u8 *mac, u16 *vmid)
{
	struct sparx5_rr_neigh_entry *nh_neigh = nh->neigh_entry;

	nh->trapped = !nh_neigh || is_zero_ether_addr(nh_neigh->hwaddr);

	if (nh_neigh) {
		memcpy(mac, nh_neigh->hwaddr, ETH_ALEN);
		*vmid = nh_neigh->vmid;
		return;
	}

	eth_zero_addr(mac);
	*vmid = 0;
}

static int
sparx5_rr_fib_entry_ecmp_hw_apply(struct sparx5 *sparx5,
				  struct sparx5_rr_fib_entry *fib_entry)
{
	struct sparx5_rr_nexthop_group_info *nhgi = fib_entry->nh_grp->nhgi;
	unsigned char mac[ETH_ALEN] __aligned(2);
	struct sparx5_rr_nexthop *nh;
	int err, i, offset;
	u16 vmid;

	offset = sparx5_rr_arp_tbl_grp_alloc(sparx5, nhgi->count);

	if (offset < 0) {
		fib_entry->offload_fail = true;
		return offset;
	}

	for (i = 0; i < nhgi->count; i++) {
		nh = &nhgi->nexthops[i];

		sparx5_rr_nexthop_egress_derive(nh, mac, &vmid);

		sparx5_rr_arp_tbl_hw_addr_apply(sparx5, mac, vmid, offset + i);
	}

	err = sparx5_rr_lpm_arp_ptr_create(sparx5,
					   &fib_entry->key.addr,
					   fib_entry->key.prefix_len, offset,
					   nhgi->count, &fib_entry->hw_route);
	if (err)
		goto err_arp_ptr_create;

	nhgi->atbl_offset = offset;
	nhgi->atbl_offset_valid = true;
	fib_entry->offload_fail = false;

	return 0;

err_arp_ptr_create:
	for (i--; i >= 0; i--)
		sparx5_rr_arp_tbl_hw_addr_clear(sparx5, offset + i);
	sparx5_rr_arp_tbl_grp_free(sparx5, offset, nhgi->count);
	fib_entry->offload_fail = true;
	nhgi->atbl_offset_valid = false;

	return err;
}

static u16 sparx5_rr_blackhole_vmid(struct sparx5 *sparx5)
{
	/* Reserve last vmid for blackhole leg. */
	return sparx5->data->consts.vmid_cnt - 1;
}

static u16 sparx5_rr_blackhole_vid(struct sparx5 *sparx5)
{
	/* Reserve VID 4096 for blackhole leg. The HW VLAN table has entries
	 * beyond 4095, for internal use.
	 */
	return VLAN_N_VID;
}

static int
sparx5_rr_fib_blackhole_hw_apply(struct sparx5 *sparx5,
				 struct sparx5_rr_fib_entry *fib_entry)
{
	u16 bh_vmid = sparx5_rr_blackhole_vmid(sparx5);
	unsigned char mac[ETH_ALEN];

	/* Hardware blackholes are implemented by:
	 *
	 * 1) Making sure traffic is not trapped with non-zero dmac.
	 * 2) Using reserved router leg vmid for egress.
	 * 3) This router leg is attached to a VLAN id > 4095.
	 * 4) The port-mask for this VLAN is all zero.
	 *
	 * The hardware VLAN table has more than 4096 entries. The specific size
	 * depends on the chip. LAN969x has 4608 and Sparx5 has 5120 entries.
	 * These additional VLAN entries can be used for internal logic.
	 *
	 * The port-mask for the blackhole VLAN is zero. Therefore, frames routed
	 * to the blackhole leg will not egress on any ports.
	 */
	eth_zero_addr(mac);
	mac[5] = 0xff;

	return sparx5_rr_lpm_arp_entry_create(sparx5, &fib_entry->key.addr,
					      fib_entry->key.prefix_len, mac,
					      bh_vmid, &fib_entry->hw_route);
}

static int sparx5_rr_fib_trap_hw_apply(struct sparx5 *sparx5,
				       struct sparx5_rr_fib_entry *fib_entry)
{
	unsigned char zero_mac[ETH_ALEN];
	u16 vmid = 0; /* VMID does not matter */

	/* Trap frames with zero mac */
	eth_zero_addr(zero_mac);

	return sparx5_rr_lpm_arp_entry_create(sparx5, &fib_entry->key.addr,
					      fib_entry->key.prefix_len,
					      zero_mac, vmid,
					      &fib_entry->hw_route);
}

static int sparx5_rr_fib_nexthop_hw_apply(struct sparx5 *sparx5,
					  struct sparx5_rr_fib_entry *fib_entry,
					  struct sparx5_rr_nexthop *nh)
{
	unsigned char mac[ETH_ALEN];
	u16 vmid;

	sparx5_rr_nexthop_egress_derive(nh, mac, &vmid);

	return sparx5_rr_lpm_arp_entry_create(sparx5, &fib_entry->key.addr,
					      fib_entry->key.prefix_len, mac,
					      vmid, &fib_entry->hw_route);
}

static int sparx5_rr_fib_entry_hw_apply(struct sparx5 *sparx5,
					struct sparx5_rr_fib_entry *fib_entry)
{
	struct sparx5_rr_nexthop_group_info *nhgi = fib_entry->nh_grp->nhgi;
	unsigned char zero_mac[ETH_ALEN] __aligned(2);
	struct sparx5_rr_nexthop *nh;
	int err = 0;

	/* Trap frames with zero mac */
	eth_zero_addr(zero_mac);

	sparx5_rr_fib_debug(sparx5, fib_entry, "apply");

	switch (fib_entry->type) {
	case SPARX5_RR_FIB_TYPE_UNREACHABLE:
		fallthrough;
	case SPARX5_RR_FIB_TYPE_PROHIBIT:
		/* Ensure kernel can respond with correct ICMP packets. */
		fallthrough;
	case SPARX5_RR_FIB_TYPE_LOCAL:
		/* Trap traffic destined for device itself, to ensure
		 * device can receive traffic even when default gateways are
		 * configured.
		 */

		/* Mark kernel fib as trapped. */
		fib_entry->trap = true;

		err = sparx5_rr_fib_trap_hw_apply(sparx5, fib_entry);
		goto out;

	case SPARX5_RR_FIB_TYPE_UNICAST:
		fib_entry->trap = false;

		if (!nhgi->nexthops->gateway) {
			/* Directly connected subnet. Trap traffic so kernel
			 * can perform ARP/NDP on our behalf.
			 */
			err = sparx5_rr_fib_trap_hw_apply(sparx5, fib_entry);
			goto out;
		}

		if (nhgi->count == 1) { /* Use arp_entry */
			nh = &nhgi->nexthops[0];
			err = sparx5_rr_fib_nexthop_hw_apply(sparx5,
							     fib_entry,
							     nh);
			goto out;
		}

		/* Multiple nexthops so we use the HW arp table. */
		err = sparx5_rr_fib_entry_ecmp_hw_apply(sparx5,
							fib_entry);
		goto out;

		break;
	case SPARX5_RR_FIB_TYPE_BLACKHOLE:
		fib_entry->trap = false;
		err = sparx5_rr_fib_blackhole_hw_apply(sparx5, fib_entry);
		goto out;

	default:
		dev_warn(sparx5->dev, "Fib entry offload, unhandled type=%d\n",
			 fib_entry->type);
		return -EINVAL;
	}

out:
	fib_entry->offload_fail = !!err;

	return err;
}

static void sparx5_rr_nexthop_neigh_update(struct sparx5 *sparx5,
					   struct sparx5_rr_nexthop *nh,
					   bool entry_connected)
{
	unsigned char mac[ETH_ALEN] __aligned(2);
	u16 vmid = nh->neigh_entry->vmid;
	int err, nh_offset, grp_idx;

	if (!nh->gateway)
		return;

	/* Trap traffic with zero mac */
	eth_zero_addr(mac);

	if (entry_connected)
		ether_addr_copy(mac, nh->neigh_entry->hwaddr);

	if (nh->trapped && !entry_connected)
		return;

	nh->trapped = !entry_connected;

	if (nh->grp->nhgi->count == 1) {
		err = sparx5_rr_fib_entry_update_arp_entry(sparx5,
							   nh->grp->fib_entry,
							   mac, vmid);
		if (err)
			dev_err(sparx5->dev,
				"Nexthop fib entry update failed\n");

		return;
	}

	nh_offset = (int)(ptrdiff_t)(nh - nh->grp->nhgi->nexthops);
	grp_idx = nh->grp->nhgi->atbl_offset;

	sparx5_rr_arp_tbl_hw_addr_apply(sparx5, mac, vmid, grp_idx + nh_offset);
}

static void
sparx5_rr_nexthops_update_notify(struct sparx5 *sparx5,
				 struct sparx5_rr_neigh_entry *neigh_entry,
				 bool entry_connected)
{
	struct sparx5_rr_nexthop *nh;

	list_for_each_entry(nh, &neigh_entry->nexthop_list, neigh_list_node)
		sparx5_rr_nexthop_neigh_update(sparx5, nh, entry_connected);
}

static int sparx5_rr_neigh_entry_hw_apply(struct sparx5 *sparx5,
					  struct sparx5_rr_neigh_entry *entry)
{
	u32 prefix_len = SPARX5_IADDR_LEN(entry->key.iaddr.version);

	if (!entry->hw_route.vrule_id_valid)
		return sparx5_rr_lpm_arp_entry_create(sparx5,
						      &entry->key.iaddr,
						      prefix_len,
						      entry->hwaddr,
						      entry->vmid,
						      &entry->hw_route);

	return sparx5_rr_lpm_arp_entry_mod(sparx5, entry->hwaddr, entry->vmid,
					    entry->hw_route.vrule_id);
}

static void sparx5_rr_neigh_entry_update(struct sparx5 *sparx5,
					 struct sparx5_rr_neigh_entry *entry,
					 bool adding)
{
	struct net_device *pdev = sparx5->router->port_dev;
	bool offloaded = adding;
	int err;

	if (!adding && !entry->connected && !entry->hw_route.vrule_id_valid)
		return;

	entry->connected = adding;

	sparx5_rr_neigh_debug(sparx5, entry, "update");

	if (adding) {
		err = sparx5_rr_neigh_entry_hw_apply(sparx5, entry);
		if (err)
			offloaded = false;
	} else if (entry->hw_route.vrule_id_valid) {
		vcap_del_rule(sparx5->vcap_ctrl, pdev, entry->hw_route.vrule_id);
		entry->hw_route.vrule_id_valid = false;
	}

	return sparx5_rr_neigh_entry_offload_mark(entry, offloaded);
}

static void sparx5_rr_fib_entry_destroy(struct sparx5 *sparx5,
					struct sparx5_rr_fib_entry *fib_entry)
{
	struct net_device *pdev = sparx5->router->port_dev;
	struct sparx5_rr_neigh_entry *neigh_entry, *tmp;
	struct vcap_control *vctrl = sparx5->vcap_ctrl;

	sparx5_rr_fib_lpm_remove(fib_entry);

	list_for_each_entry_safe(neigh_entry, tmp, &fib_entry->neigh_list,
				 fib_list_node) {
		list_del(&neigh_entry->fib_list_node);
		neigh_entry->fib_entry = NULL;

		/* Remove LPM VCAP entry for neighbour, if used */
		sparx5_rr_neigh_entry_update(sparx5, neigh_entry, false);
		sparx5_rr_nexthops_update_notify(sparx5, neigh_entry, false);
		sparx5_rr_neigh_entry_put(sparx5, neigh_entry);
	}

	sparx5_rr_fib_debug(sparx5, fib_entry, "destroy");

	sparx5_rr_fib_entry_remove(sparx5, fib_entry);
	sparx5_rr_nexthop_group_put(sparx5, fib_entry->nh_grp);
	if (fib_entry->hw_route.vrule_id_valid)
		vcap_del_rule(vctrl, pdev, fib_entry->hw_route.vrule_id);
	sparx5_rr_fib_info_put(&fib_entry->fi);
	kfree(fib_entry);
}

/* Update nexthop group based on current fib_info state. */
static int
sparx5_rr_entry_nexthop_group_update(struct sparx5 *sparx5,
				     struct sparx5_rr_fib_entry *fib_entry)
{
	struct net_device *pdev = sparx5->router->port_dev;
	struct vcap_control *vctrl = sparx5->vcap_ctrl;
	struct sparx5_rr_nexthop_group *new_nh_grp;
	struct sparx5_rr_nexthop_group *old_nh_grp;
	u32 old_vrule_id;
	int err;

	old_nh_grp = fib_entry->nh_grp;
	old_vrule_id = fib_entry->hw_route.vrule_id;

	/* Prepare new group in SW representation */
	new_nh_grp = sparx5_rr_nexthop_group_create(sparx5, fib_entry);
	if (IS_ERR(new_nh_grp)) {
		dev_warn(sparx5->dev, "Failed to create nexthop group\n");
		return PTR_ERR(new_nh_grp);
	}

	fib_entry->nh_grp = new_nh_grp;
	new_nh_grp->fib_entry = fib_entry;

	/* Write new rule to HW */
	err = sparx5_rr_fib_entry_hw_apply(sparx5, fib_entry);
	if (err)
		goto hw_apply_err;

	/* Clean up old rule and start routing traffic according to new rule */
	if (fib_entry->hw_route.vrule_id != old_vrule_id)
		vcap_del_rule(vctrl, pdev, old_vrule_id);

	/* Remove old unused group */
	sparx5_rr_nexthop_group_put(sparx5, old_nh_grp);

	return 0;

hw_apply_err:
	fib_entry->nh_grp = old_nh_grp;
	new_nh_grp->fib_entry = NULL;
	sparx5_rr_nexthop_group_put(sparx5, new_nh_grp);
	return err;
}

static void sparx5_rr_leg_hw_init(struct sparx5 *sparx5,
				  struct sparx5_rr_router_leg *leg)
{
	/* Associate Router leg VMID to VLAN */
	spx5_rmw(ANA_L3_VMID_CFG_VMID_SET(leg->vmid), ANA_L3_VMID_CFG_VMID,
		 sparx5, ANA_L3_VMID_CFG(leg->vid));

	/* Enable Router leg for VLAN */
	spx5_rmw(ANA_L3_VLAN_CFG_VLAN_RLEG_ENA_SET(1),
		 ANA_L3_VLAN_CFG_VLAN_RLEG_ENA, sparx5,
		 ANA_L3_VLAN_CFG(leg->vid));

	/* Configure router leg */

#if IS_ENABLED(CONFIG_IPV6)
	spx5_rmw(ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA_SET(1) |
		 ANA_L3_RLEG_CTRL_RLEG_EVID_SET(leg->vid) |
		 ANA_L3_RLEG_CTRL_RLEG_IP6_UC_ENA_SET(1),
		 ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA |
		 ANA_L3_RLEG_CTRL_RLEG_EVID |
		 ANA_L3_RLEG_CTRL_RLEG_IP6_UC_ENA, sparx5,
		 ANA_L3_RLEG_CTRL(leg->vmid));
#else
	spx5_rmw(ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA_SET(1) |
		 ANA_L3_RLEG_CTRL_RLEG_EVID_SET(leg->vid),
		 ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA |
		 ANA_L3_RLEG_CTRL_RLEG_EVID, sparx5,
		 ANA_L3_RLEG_CTRL(leg->vmid));
#endif

	/* Configure egress VLAN in rewriter */
	spx5_rmw(REW_RLEG_CTRL_RLEG_EVID_SET(leg->vid), REW_RLEG_CTRL_RLEG_EVID,
		 sparx5, REW_RLEG_CTRL(leg->vmid));
}

static void sparx5_rr_leg_hw_deinit(struct sparx5 *sparx5,
				    struct sparx5_rr_router_leg *leg)
{
	/* Disable Router leg for VLAN */
	spx5_rmw(ANA_L3_VLAN_CFG_VLAN_RLEG_ENA_SET(0),
		 ANA_L3_VLAN_CFG_VLAN_RLEG_ENA, sparx5,
		 ANA_L3_VLAN_CFG(leg->vid));

	/* Disable IP UC routing on leg */
	spx5_rmw(ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA_SET(0) |
		 ANA_L3_RLEG_CTRL_RLEG_IP6_UC_ENA_SET(0),
		 ANA_L3_RLEG_CTRL_RLEG_IP4_UC_ENA |
		 ANA_L3_RLEG_CTRL_RLEG_IP6_UC_ENA, sparx5,
		 ANA_L3_RLEG_CTRL(leg->vmid));
}

static int sparx5_rr_lpm_link_local_create(struct sparx5 *sparx5)
{
	struct sparx5_iaddr addr __aligned(2) = { 0 };
	unsigned char zero_mac[ETH_ALEN];

	eth_zero_addr(zero_mac);

	/* Trap traffic to fe80::/64 */
	addr.version = SPARX5_IPV6;
	addr.ipv6.in6_u.u6_addr8[0] = 0xfe;
	addr.ipv6.in6_u.u6_addr8[1] = 0x80;

	return sparx5_rr_lpm_arp_entry_create(sparx5, &addr,
					      SPARX5_LINK_LOCAL_PREFIX_LEN,
					      zero_mac, 0,
					      &sparx5->router->link_local);
}

static void sparx5_rr_lpm_link_local_destroy(struct sparx5 *sparx5)
{
	struct sparx5_rr_hw_route *llocal = &sparx5->router->link_local;
	struct net_device *pdev = sparx5->router->port_dev;
	struct vcap_control *vctrl = sparx5->vcap_ctrl;

	if (!llocal->vrule_id_valid)
		return;

	vcap_del_rule(vctrl, pdev, llocal->vrule_id);
	llocal->vrule_id_valid = false;
}

static struct sparx5_rr_router_leg *
__sparx5_rr_leg_alloc(struct sparx5 *sparx5, struct net_device *dev, u16 vmid,
		      u16 vid)
{
	struct sparx5_rr_router_leg *leg;

	leg = kzalloc(sizeof(*leg), GFP_KERNEL);
	if (!leg)
		return NULL;

	INIT_LIST_HEAD(&leg->leg_list_node);
	leg->dev = dev;
	leg->vmid = vmid;
	leg->vid = vid;
	leg->sparx5 = sparx5;

	return leg;
}

/* Router legs are identified by their VMID in hw */
static struct sparx5_rr_router_leg *
sparx5_rr_leg_alloc(struct sparx5 *sparx5, struct net_device *dev, u16 vid)
{
	struct sparx5_rr_router_leg *leg;
	int next_vmid;

	next_vmid = sparx5_vmid_alloc(sparx5);
	if (next_vmid < 0)
		return NULL;

	leg = __sparx5_rr_leg_alloc(sparx5, dev, next_vmid, vid);
	if (!leg)
		goto err_kzalloc;

	return leg;

err_kzalloc:
	sparx5_vmid_free(sparx5, next_vmid);

	return NULL;
}

static void sparx5_rr_router_leg_destroy(struct sparx5_rr_router_leg *leg)
{
	struct sparx5 *sparx5 = leg->sparx5;

	dev_dbg(sparx5->dev, "Leg destroy vid=%u vmid=%u dev=%s\n", leg->vid,
		leg->vmid, leg->dev->name);

	sparx5_rr_leg_hw_deinit(sparx5, leg);
	sparx5_vmid_free(leg->sparx5, leg->vmid);
	list_del(&leg->leg_list_node);

	if (atomic_dec_return(&sparx5->router->legs_count) == 0)
		sparx5_rr_lpm_link_local_destroy(sparx5);

	netdev_put(leg->dev, NULL);
	kfree(leg);
}

static struct sparx5_rr_router_leg *
sparx5_rr_router_leg_create(struct sparx5 *sparx5, struct net_device *dev,
			    u16 vid)
{
	struct sparx5_rr_router_leg *leg;

	leg = sparx5_rr_leg_alloc(sparx5, dev, vid);
	if (!leg)
		return ERR_PTR(-ENOMEM);

	/* Prevent net device from being freed while we have added it to a router
	 * leg.
	 */
	netdev_hold(dev, NULL, GFP_KERNEL);

	/* While a router leg exists, add route to trap link-local traffic. */
	if (atomic_inc_return(&sparx5->router->legs_count) == 1) {
		if (sparx5_rr_lpm_link_local_create(sparx5))
			dev_warn(sparx5->dev,
				 "Failed to create link-local route\n");
	}

	list_add(&leg->leg_list_node, &sparx5->router->leg_list);
	sparx5_rr_leg_hw_init(sparx5, leg);

	dev_dbg(sparx5->dev, "Leg create dev=%s vid=%u vmid=%u\n", dev->name,
		leg->vid, leg->vmid);

	return leg;
}

static void sparx5_rr_fib4_del(struct sparx5 *sparx5,
			       struct sparx5_rr_fib_info *fi)
{
	struct sparx5_rr_fib_entry *fib_entry;
	struct sparx5_rr_fib_key key;

	sparx5_rr_fib_info_to_fib_key(fi, &key);

	fib_entry = sparx5_rr_fib_entry_lookup(sparx5, &key);
	if (!fib_entry)
		return;

	sparx5_rr_fib_entry_destroy(sparx5, fib_entry);
}

static bool sparx5_rr_dev_real_is_vlan_aware(struct net_device *dev)
{
	struct net_device *vlan_rdev;
	/* Support l3 offloading for:
	 *	1) upper vlan interfaces for the bridge.
	 */
	if (is_vlan_dev(dev)) {
		if (netif_is_bridge_port(dev))
			return false;

		vlan_rdev = vlan_dev_real_dev(dev);
		if (sparx5_netdevice_check(vlan_rdev))
			return false;

		return netif_is_bridge_master(vlan_rdev) &&
		       br_vlan_enabled(vlan_rdev);
	}
	return false;
}

static bool sparx5_rr_fib_info_should_offload(struct sparx5 *sparx5,
					      struct sparx5_rr_fib_info *fi)
{
	u32 tb_id = sparx5_rr_fib_info_tb_id(fi);
	u8 type = sparx5_rr_fib_info_type(fi);
	int nhs = sparx5_rr_fib_info_nhs(fi);

	if (!(type == RTN_UNICAST ||
	      type == RTN_LOCAL ||
	      type == RTN_BLACKHOLE ||
	      type == RTN_PROHIBIT ||
	      type == RTN_UNREACHABLE))
		return false;

	if (!(tb_id == RT_TABLE_MAIN ||
	      tb_id == RT_TABLE_LOCAL))
		return false;

	/* No support for nexthop objects (optimization for larger scale
	 * routing). Instead each route has a copy of it's nexthops.
	 */
	if (sparx5_rr_fib_info_is_nh_obj(fi))
		return false;

	/* For IPv4 the nexthops of these route types have NULL egress device.
	 * However, for IPv6 the nexthops use the loopback interface, so accept
	 * early.
	 */
	if (type == RTN_BLACKHOLE ||
	    type == RTN_PROHIBIT ||
	    type == RTN_UNREACHABLE)
		return true;

	if (nhs > SPARX5_MAX_ECMP_SIZE)
		return false;

	for (int i = 0; i < nhs; i++) {
		struct fib_nh_common *nhc = sparx5_rr_fib_info_nhc(fi, i);

		if (nhc->nhc_dev &&
		    !sparx5_rr_dev_real_is_vlan_aware(nhc->nhc_dev))
			return false;

		/* HW only supports equal weight nexthops */
		if (nhc->nhc_weight != 1)
			return false;
	}

	return true;
}

static int sparx5_rr_fib_replace(struct sparx5 *sparx5,
				 struct sparx5_rr_fib_info *fi)
{
	u8 new_fib_type, fi_type = sparx5_rr_fib_info_type(fi);
	struct sparx5_rr_nexthop_group *old_nh_grp;
	struct sparx5_rr_fib_entry *fib_entry;
	struct sparx5_rr_fib_key key;
	int err = 0;

	if (sparx5_rr_fib_info_should_ignore(fi))
		return 0;

	sparx5_rr_fib_info_to_fib_key(fi, &key);

	fib_entry = sparx5_rr_fib_entry_lookup(sparx5, &key);

	if (!sparx5_rr_fib_info_should_offload(sparx5, fi)) {
		/* A previously offloadable fib, is modified to unoffloadable
		 * state, so we must remove it.
		 */
		if (fib_entry)
			sparx5_rr_fib_entry_destroy(sparx5, fib_entry);
		return 0;
	}

	if (!fib_entry) {
		/* Holds refs to kernel fib_info */
		fib_entry = sparx5_rr_fib_entry_create(sparx5, &key, fi);
		if (IS_ERR(fib_entry)) {
			dev_warn(sparx5->dev, "Failed to create fib entry\n");
			sparx5_rr_fib_info_offload_mark(sparx5, fi, false,
							false, true);
			return PTR_ERR(fib_entry);
		}

		err = sparx5_rr_fib_entry_hw_apply(sparx5, fib_entry);
		goto out_fib_mark_offload;
	}

	old_nh_grp = fib_entry->nh_grp;

	/* Release and allow any previous fib_info to be deleted */
	sparx5_rr_fib_info_put(&fib_entry->fi);

	/* Hold and replace with new fib_info */
	err = sparx5_rr_fib_entry_fib_info_add(fib_entry, fi);
	if (err) {
		dev_err(sparx5->dev, "Failed to replace fib info\n");
		goto out_fib_mark_offload;
	}

	new_fib_type = sparx5_rr_rtm_type2fib_type(fi_type);

	/* Nexthop group did not change, so skip group reallocation. */
	if (fib_entry->type == new_fib_type &&
	    sparx5_rr_fib_info_nh_group_equal(old_nh_grp, &fib_entry->fi))
		goto out_fib_mark_offload;

	fib_entry->type = new_fib_type;

	/* Fib's nexthop group changed, so we must update it */
	err = sparx5_rr_entry_nexthop_group_update(sparx5, fib_entry);

out_fib_mark_offload:
	fib_entry->offload_fail = !!err;
	sparx5_rr_fib_entry_offload_mark(sparx5, fib_entry);
	if (err)
		sparx5_rr_fib_entry_destroy(sparx5, fib_entry);
	return err;
}

static void sparx5_rr_fib4_event_work(struct work_struct *work)
{
	struct sparx5_fib_event_work *fib_work =
		container_of(work, struct sparx5_fib_event_work, work);
	struct sparx5 *sparx5 = fib_work->sparx5;
	int err;

	mutex_lock(&sparx5->router->lock);

	switch (fib_work->event) {
	case FIB_EVENT_ENTRY_REPLACE:
		err = sparx5_rr_fib_replace(sparx5, &fib_work->fi);
		if (err)
			dev_warn(sparx5->dev, "FIB replace failed, ip=%pI4l\n",
				 &fib_work->fi.fen4_info.dst);

		break;
	case FIB_EVENT_ENTRY_DEL:
		sparx5_rr_fib4_del(sparx5, &fib_work->fi);
		break;
	default:
		/* FIB_EVENT_ENTRY_APPEND only occurs for IPv6. */
		WARN_ON_ONCE(1); /* BUG */
		break;
	}

	/* Release fib_info hold for workqueue. */
	sparx5_rr_fib_info_put(&fib_work->fi);
	mutex_unlock(&sparx5->router->lock);
	kfree(fib_work);
}

static int sparx5_rr_fib6_append(struct sparx5 *sparx5,
				 struct sparx5_rr_fib_info *fi)
{
	struct sparx5_rr_fib_entry *fib_entry;
	struct sparx5_rr_fib_key key;
	int err = 0;

	if (sparx5_rr_fib_info_should_ignore(fi))
		return 0;

	sparx5_rr_fib_info_to_fib_key(fi, &key);

	fib_entry = sparx5_rr_fib_entry_lookup(sparx5, &key);
	if (!fib_entry)
		return 0;

	/* Are we adding new nexthops which can not be offloaded */
	if (!sparx5_rr_fib_info_should_offload(sparx5, fi)) {
		err = -EINVAL;
		goto out_fib_mark_offload;
	}

	/* Append new rt_arr data to fen6_info rt data */
	err = sparx5_rr_fib_entry_fib_info_add(fib_entry, fi);
	if (err)
		goto out_fib_mark_offload;

	/* Realloc nexthop group and apply to hw. */
	err = sparx5_rr_entry_nexthop_group_update(sparx5, fib_entry);

	sparx5_rr_fib_debug(sparx5, fib_entry, "append");

out_fib_mark_offload:
	fib_entry->offload_fail = !!err;
	sparx5_rr_fib_entry_offload_mark(sparx5, fib_entry);
	if (err)
		sparx5_rr_fib_entry_destroy(sparx5, fib_entry);

	return err;
}

static bool sparx5_rr_fib6_rt_exists(struct sparx5_rr_fib6_entry_info *f6i,
				     struct fib6_info *rt)
{
	for (int i = 0; i < f6i->nrt6; i++)
		if (f6i->rt_arr[i] == rt)
			return true;

	return false;
}

static int sparx5_rr_fib6_nexthop_prune(struct sparx5 *sparx5,
					struct sparx5_rr_fib_entry *fib_entry,
					struct sparx5_rr_fib6_entry_info *f6i)
{
	struct fib6_info **old_rt_arr = fib_entry->fi.fe6_info.rt_arr;
	unsigned int old_nrt6 = fib_entry->fi.fe6_info.nrt6;
	unsigned int new_nrt6 = old_nrt6 >= f6i->nrt6 ? old_nrt6 - f6i->nrt6 :
							0;
	struct fib6_info **rt_arr;
	int j = 0;

	rt_arr = kcalloc(new_nrt6, sizeof(struct fib6_info *), GFP_KERNEL);
	if (!rt_arr)
		return -ENOMEM;

	for (int i = 0; i < old_nrt6; i++) {
		struct fib6_info *fi = old_rt_arr[i];

		if (sparx5_rr_fib6_rt_exists(f6i, fi)) {
			sparx5_rr_rt6_release(fi);
			continue;
		}

		rt_arr[j++] = fi;
	}

	/* Assume incoming f6i only contain live nexthops, and no duplicates. */
	WARN_ON_ONCE(j != new_nrt6);

	kfree(fib_entry->fi.fe6_info.rt_arr);
	fib_entry->fi.fe6_info.nrt6 = new_nrt6;
	fib_entry->fi.fe6_info.rt_arr = rt_arr;
	return 0;
}

static int sparx5_rr_fib6_del(struct sparx5 *sparx5,
			      struct sparx5_rr_fib_info *fi)
{
	struct sparx5_rr_fib_entry *fib_entry;
	int nhs = sparx5_rr_fib_info_nhs(fi);
	struct sparx5_rr_fib_key key;
	int err;

	sparx5_rr_fib_info_to_fib_key(fi, &key);

	fib_entry = sparx5_rr_fib_entry_lookup(sparx5, &key);
	if (!fib_entry)
		return 0;

	/* Full delete. */
	if (nhs == sparx5_rr_fib_info_nhs(&fib_entry->fi)) {
		sparx5_rr_fib_entry_destroy(sparx5, fib_entry);
		return 0;
	}

	/* Partial delete. Remove fi nexthops from fib_entry. */
	err = sparx5_rr_fib6_nexthop_prune(sparx5, fib_entry, &fi->fe6_info);
	if (err)
		goto err_nexthop_prune;

	/* Realloc nexthop group and apply to hw. */
	err = sparx5_rr_entry_nexthop_group_update(sparx5, fib_entry);

	sparx5_rr_fib_debug(sparx5, fib_entry, "prune");

err_nexthop_prune:
	fib_entry->offload_fail = !!err;
	sparx5_rr_fib_entry_offload_mark(sparx5, fib_entry);
	if (err)
		sparx5_rr_fib_entry_destroy(sparx5, fib_entry);

	return err;
}

static void sparx5_rr_fib6_event_work(struct work_struct *work)
{
	struct sparx5_fib_event_work *fib_work =
		container_of(work, struct sparx5_fib_event_work, work);
	struct sparx5_rr_fib_info *fi = &fib_work->fi;
	struct sparx5 *sparx5 = fib_work->sparx5;
	int err;

	mutex_lock(&sparx5->router->lock);

	switch (fib_work->event) {
	case FIB_EVENT_ENTRY_REPLACE:
		err = sparx5_rr_fib_replace(sparx5, fi);
		if (err)
			dev_warn(sparx5->dev, "FIB 6 replace failed.\n");

		break;

	case FIB_EVENT_ENTRY_APPEND:
		/* Netlink API for IPv6 is different from IPV4. It is possible to
		 * do partial update/deletes of nexthops on a route. In this case
		 * fi only contains the nexthops to add/remove, and must be
		 * merged with the existing nexthops on the route.
		 * Therefore, we only share fib_replace between IPv6 and IPv4
		 * logic.
		 */
		err = sparx5_rr_fib6_append(sparx5, fi);
		if (err)
			dev_warn(sparx5->dev, "FIB 6 append failed.\n");

		break;

	case FIB_EVENT_ENTRY_DEL:
		err = sparx5_rr_fib6_del(sparx5, fi);
		if (err)
			dev_warn(sparx5->dev, "FIB 6 delete failed.\n");

		break;

	default:
		WARN_ON_ONCE(1); /* BUG */
		break;
	}

	/* Release fib6_info holds for workqueue. */
	sparx5_rr_fib_info_put(fi);
	mutex_unlock(&sparx5->router->lock);
	kfree(fib_work);
}

static int sparx5_rr_fib6_work_init(struct sparx5_fib_event_work *fib_work,
				    struct fib6_entry_notifier_info *fen6_info)
{
	struct sparx5_rr_fib6_entry_info *fib6_info = &fib_work->fi.fe6_info;
	struct fib6_info *rt = fen6_info->rt;
	struct fib6_info **rt_arr;
	struct fib6_info *iter;
	unsigned int nrt6;
	int i = 0;

	nrt6 = fen6_info->nsiblings + 1;

	rt_arr = kcalloc(nrt6, sizeof(struct fib6_info *), GFP_ATOMIC);
	if (!rt_arr)
		return -ENOMEM;

	fib6_info->rt_arr = rt_arr;
	fib6_info->nrt6 = nrt6;

	rt_arr[0] = rt;
	fib6_info_hold(rt);

	if (!fen6_info->nsiblings)
		return 0;

	list_for_each_entry(iter, &rt->fib6_siblings, fib6_siblings) {
		if (i == fen6_info->nsiblings)
			break;

		rt_arr[i + 1] = iter;
		fib6_info_hold(iter);
		i++;
	}

	return 0;
}

/* Handle fib events, which manage fib_entries. Called in atomic context, with
 * rcu_read_lock().
 */
static int sparx5_rr_fib_event(struct notifier_block *nb, unsigned long event,
			       void *ptr)
{
	struct fib6_entry_notifier_info *fen6_info;
	struct fib_entry_notifier_info *fen_info;
	struct sparx5_fib_event_work *fib_work;
	struct fib_notifier_info *info = ptr;
	struct sparx5_router *router;
	int err;

	/* Handle IPv4 and IPv6  */
	if (info->family != AF_INET && info->family != AF_INET6)
		return NOTIFY_DONE;

	if (event != FIB_EVENT_ENTRY_REPLACE &&
	    event != FIB_EVENT_ENTRY_DEL &&
	    event != FIB_EVENT_ENTRY_APPEND)
		return NOTIFY_DONE;

	router = container_of(nb, struct sparx5_router, fib_nb);

	fib_work = kzalloc(sizeof(*fib_work), GFP_ATOMIC);
	if (!fib_work)
		return NOTIFY_BAD;

	fib_work->sparx5 = router->sparx5;
	fib_work->event = event;

	switch (info->family) {
	case AF_INET:
		INIT_WORK(&fib_work->work, sparx5_rr_fib4_event_work);

		fen_info = container_of(info, struct fib_entry_notifier_info,
					info);
		fib_work->fi.fen4_info = *fen_info;
		fib_work->fi.version = SPARX5_IPV4;

		/* Hold fib_info while item is queued */
		fib_info_hold(fib_work->fi.fen4_info.fi);

		sparx5_rr_schedule_work(router->sparx5, &fib_work->work);
		break;
	case AF_INET6:
		INIT_WORK(&fib_work->work, sparx5_rr_fib6_event_work);

		/* Copy and hold fib6_info for route and all nhs while item is
		 * queued.
		 */
		fen6_info = container_of(info, struct fib6_entry_notifier_info,
					 info);
		err = sparx5_rr_fib6_work_init(fib_work, fen6_info);
		if (err)
			goto err_fib6;

		fib_work->fi.version = SPARX5_IPV6;

		sparx5_rr_schedule_work(router->sparx5, &fib_work->work);
		break;
	default:
		goto err_fam_unhandled;
	}

	return NOTIFY_DONE;

err_fam_unhandled:
	WARN_ON_ONCE(1); /* BUG */
err_fib6:
	kfree(fib_work);
	return NOTIFY_BAD;
}

static void sparx5_rr_neigh_event_work(struct work_struct *work)
{
	struct sparx5_rr_netevent_work *net_work =
		container_of(work, struct sparx5_rr_netevent_work, work);
	unsigned char hwaddr[ETH_ALEN] __aligned(2);
	struct sparx5 *sparx5 = net_work->sparx5;
	struct sparx5_rr_neigh_key key = { 0 };
	struct neighbour *n = net_work->neigh;
	struct sparx5_rr_neigh_entry *entry;
	bool entry_connected;
	u8 nud_state, dead;

	sparx5_rr_nb2neigh_key(n, &key);

	/* Frames with link-local dip are trapped, so ignore the neighbour. */
	if (sparx5_rr_addr_is_link_local(&key.iaddr))
		goto out;

	/* If n changes after this read section, we will get another neigh event,
	 * which is processed after the current one.
	 */
	read_lock_bh(&n->lock);
	ether_addr_copy(hwaddr, n->ha);
	nud_state = n->nud_state;
	dead = n->dead;
	read_unlock_bh(&n->lock);

	mutex_lock(&sparx5->router->lock);

	entry_connected = nud_state & NUD_VALID && !dead;
	entry = sparx5_rr_neigh_entry_lookup(sparx5, &key);
	if (!entry_connected && !entry)
		goto out_mutex;

	if (!entry) {
		entry = sparx5_rr_neigh_entry_create(sparx5, &key);
		if (IS_ERR(entry))
			goto out_mutex;
	}

	if (entry->connected && entry_connected &&
	    ether_addr_equal(entry->hwaddr, hwaddr))
		goto out_mutex;

	ether_addr_copy(entry->hwaddr, hwaddr);
	sparx5_rr_neigh_entry_update(sparx5, entry, entry_connected);
	sparx5_rr_nexthops_update_notify(sparx5, entry, entry_connected);
	if (!entry_connected)
		sparx5_rr_neigh_entry_put(sparx5, entry);

out_mutex:
	mutex_unlock(&sparx5->router->lock);
out:
	neigh_release(n);
	kfree(net_work);
}

/* Handle neighbour update events. Used to manage neigh_entries. Called in atomic
 * context, with rcu_read_lock().
 */
static int sparx5_rr_netevent_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct sparx5_rr_netevent_work *net_work;
	struct sparx5_router *router;
	struct sparx5_port *port;
	struct neighbour *n;

	router = container_of(nb, struct sparx5_router, netevent_nb);

	switch (event) {
	case NETEVENT_NEIGH_UPDATE:
		n = ptr;

		if (n->tbl->family != AF_INET && n->tbl->family != AF_INET6)
			return NOTIFY_DONE;

		port = sparx5_port_dev_lower_find(n->dev);
		if (!port)
			return NOTIFY_DONE;

		net_work = kzalloc(sizeof(*net_work), GFP_ATOMIC);
		if (!net_work)
			return NOTIFY_BAD;

		neigh_clone(n);
		INIT_WORK(&net_work->work, sparx5_rr_neigh_event_work);
		net_work->sparx5 = router->sparx5;
		net_work->neigh = n;
		net_work->event = event;
		sparx5_rr_schedule_work(router->sparx5, &net_work->work);

		return NOTIFY_DONE;
	}

	return NOTIFY_DONE;
};

static void sparx5_rr_leg_base_mac_set(struct sparx5 *sparx5,
				       unsigned char mac[ETH_ALEN])
{
	u8 rleg_type_sel = SPARX5_RLEG_USE_GLOBAL_BASE_MAC;
	u32 mac_msb, mac_lsb;

	sparx5_rr_split_mac(mac, 24, &mac_msb, &mac_lsb);

	dev_dbg(sparx5->dev, "Router leg base MAC=%pM\n", mac);

	/* The global router leg MAC must be set consistently across ANA_L3, REW
	 * and EACL.
	 */
	spx5_wr(ANA_L3_RLEG_CFG_0_RLEG_MAC_LSB_SET(mac_lsb), sparx5,
		ANA_L3_RLEG_CFG_0);

	spx5_rmw(ANA_L3_RLEG_CFG_1_RLEG_MAC_MSB_SET(mac_msb) |
		 ANA_L3_RLEG_CFG_1_RLEG_MAC_TYPE_SEL_SET(rleg_type_sel),
		 ANA_L3_RLEG_CFG_1_RLEG_MAC_MSB |
		 ANA_L3_RLEG_CFG_1_RLEG_MAC_TYPE_SEL,
		 sparx5, ANA_L3_RLEG_CFG_1);

	/* Set global Router leg MAC (REW) */
	spx5_wr(REW_RLEG_CFG_0_RLEG_MAC_LSB_SET(mac_lsb), sparx5,
		REW_RLEG_CFG_0);

	spx5_rmw(REW_RLEG_CFG_1_RLEG_MAC_MSB_SET(mac_msb) |
		 REW_RLEG_CFG_1_RLEG_MAC_TYPE_SEL_SET(rleg_type_sel),
		 REW_RLEG_CFG_1_RLEG_MAC_MSB | REW_RLEG_CFG_1_RLEG_MAC_TYPE_SEL,
		 sparx5, REW_RLEG_CFG_1);

	/* Set global Router leg MAC (EACL) */
	spx5_wr(EACL_RLEG_CFG_0_RLEG_MAC_LSB_SET(mac_lsb), sparx5,
		EACL_RLEG_CFG_0);

	spx5_rmw(EACL_RLEG_CFG_1_RLEG_MAC_MSB_SET(mac_msb) |
		 EACL_RLEG_CFG_1_RLEG_MAC_TYPE_SEL_SET(rleg_type_sel),
		 EACL_RLEG_CFG_1_RLEG_MAC_MSB |
		 EACL_RLEG_CFG_1_RLEG_MAC_TYPE_SEL,
		 sparx5, EACL_RLEG_CFG_1);
}

static bool
sparx5_rr_router_leg_addr_list_empty_rcu(struct sparx5_rr_router_leg *leg)
{
	struct inet6_dev *inet6_dev;
	struct in_device *in_dev;

	in_dev = __in_dev_get_rcu(leg->dev);
	if (in_dev && in_dev->ifa_list)
		return false;

	inet6_dev = __in6_dev_get(leg->dev);
	if (inet6_dev && !list_empty(&inet6_dev->addr_list))
		return false;

	return true;
}

static bool
sparx5_rr_router_leg_addr_list_empty(struct sparx5_rr_router_leg *leg)
{
	bool addr_list_empty;

	rcu_read_lock();
	addr_list_empty = sparx5_rr_router_leg_addr_list_empty_rcu(leg);
	rcu_read_unlock();

	return addr_list_empty;
}

static int __sparx5_rr_inetaddr_event(struct sparx5 *sparx5,
				      struct net_device *dev,
				      unsigned long event)
{
	struct sparx5_rr_router_leg *leg;
	u16 vid;

	if (!sparx5_rr_dev_real_is_vlan_aware(dev))
		return 0;

	/* Our basic case: ip addr/subnet added to vlan upper of
	 * bridge dev.
	 */
	switch (event) {
	case NETDEV_UP:
		leg = sparx5_rr_leg_find_by_dev(sparx5, dev);
		if (leg)
			return 0;

		/* HW allows at most 1 leg per VLAN, but we do not need to lookup
		 * leg by vid, since the kernel does not allow multiple vlan
		 * devs with the same vid on top of a given device.
		 */
		vid = vlan_dev_vlan_id(dev);

		leg = sparx5_rr_router_leg_create(sparx5, dev, vid);
		if (IS_ERR(leg))
			return PTR_ERR(leg);
		break;
	case NETDEV_DOWN:
		leg = sparx5_rr_leg_find_by_dev(sparx5, dev);
		if (!leg || !sparx5_rr_router_leg_addr_list_empty(leg))
			return 0;

		sparx5_rr_router_leg_destroy(leg);
		break;
	}

	return 0;
}

static int sparx5_rr_inetaddr_event_handle(struct sparx5 *sparx5,
					   struct net_device *dev,
					   unsigned long event)
{
	int err;

	mutex_lock(&sparx5->router->lock);
	err = __sparx5_rr_inetaddr_event(sparx5, dev, event);
	mutex_unlock(&sparx5->router->lock);

	return notifier_from_errno(err);
}

/* Called with RTNL. */
static int sparx5_rr_inet6addr_valid_event(struct notifier_block *nb,
					   unsigned long event, void *ptr)
{
	struct in6_validator_info *i6vi = (struct in6_validator_info *)ptr;
	struct net_device *dev = i6vi->i6vi_dev->dev;
	struct sparx5_router *router;

	if (event != NETDEV_UP)
		return NOTIFY_DONE;

	router = container_of(nb, struct sparx5_router, inet6addr_valid_nb);

	return sparx5_rr_inetaddr_event_handle(router->sparx5, dev, event);
}

static void sparx5_rr_inet6addr_event_work(struct work_struct *work)
{
	struct sparx5_rr_inet6addr_event_work *addr_work =
		container_of(work, struct sparx5_rr_inet6addr_event_work, work);
	struct sparx5_router *router = addr_work->sparx5->router;

	rtnl_lock();
	mutex_lock(&router->lock);

	__sparx5_rr_inetaddr_event(addr_work->sparx5, addr_work->dev,
				   addr_work->event);

	mutex_unlock(&router->lock);
	rtnl_unlock();
	netdev_put(addr_work->dev, NULL);
	kfree(addr_work);
}

/* Called in atomic context. */
static int sparx5_rr_inet6addr_event(struct notifier_block *nb,
				     unsigned long event, void *ptr)
{
	struct inet6_ifaddr *if6 = (struct inet6_ifaddr *)ptr;
	struct sparx5_rr_inet6addr_event_work *work;
	struct net_device *dev = if6->idev->dev;
	struct sparx5_router *router;

	if (event != NETDEV_DOWN)
		return NOTIFY_DONE;

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work)
		return NOTIFY_BAD;

	router = container_of(nb, struct sparx5_router, inet6addr_nb);
	INIT_WORK(&work->work, sparx5_rr_inet6addr_event_work);
	work->sparx5 = router->sparx5;
	work->dev = dev;
	work->event = event;
	netdev_hold(dev, NULL, GFP_ATOMIC);
	sparx5_rr_schedule_work(router->sparx5, &work->work);

	return NOTIFY_DONE;
}

/* Handle events for ip address changes on ifs. Used to manage router legs.
 * Called with RTNL.
 */
static int sparx5_rr_inetaddr_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct in_ifaddr *ifa = (struct in_ifaddr *)ptr;
	struct net_device *dev = ifa->ifa_dev->dev;
	struct sparx5_router *router;

	if (event != NETDEV_DOWN)
		return NOTIFY_DONE;

	router = container_of(nb, struct sparx5_router, inetaddr_nb);

	return sparx5_rr_inetaddr_event_handle(router->sparx5, dev, event);
}

/* Called with RTNL. */
static int sparx5_rr_inetaddr_valid_event(struct notifier_block *nb,
					  unsigned long event, void *ptr)
{
	struct in_validator_info *ivi = (struct in_validator_info *)ptr;
	struct net_device *dev = ivi->ivi_dev->dev;
	struct sparx5_router *router;

	if (event != NETDEV_UP)
		return NOTIFY_DONE;

	router = container_of(nb, struct sparx5_router, inetaddr_valid_nb);

	return sparx5_rr_inetaddr_event_handle(router->sparx5, dev, event);
}

/* Called with RTNL. */
static int sparx5_rr_netdevice_event(struct notifier_block *nb,
				     unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	unsigned char mac[ETH_ALEN] __aligned(2);
	struct sparx5_router *router;
	struct sparx5 *sparx5;

	router = container_of(nb, struct sparx5_router, netdevice_nb);
	sparx5 = router->sparx5;

	/* Allow single bridge. Global router leg MAC tracks bridge mac. */
	if (!netif_is_bridge_master(dev))
		return NOTIFY_OK;

	switch (event) {
	case NETDEV_CHANGEADDR:
		ether_addr_copy(mac, dev->dev_addr);
		sparx5_rr_leg_base_mac_set(sparx5, mac);
		break;
	}

	return NOTIFY_OK;
}

static int sparx5_rr_blackhole_leg_create(struct sparx5 *sparx5)
{
	u16 vmid = sparx5_rr_blackhole_vmid(sparx5);
	u16 vid = sparx5_rr_blackhole_vid(sparx5);
	struct sparx5_rr_router_leg *leg;

	leg = __sparx5_rr_leg_alloc(sparx5, NULL, vmid, vid);
	if (!leg)
		return -ENOMEM;

	set_bit(vmid, sparx5->router->vmid_mask);

	list_add(&leg->leg_list_node, &sparx5->router->leg_list);
	sparx5_rr_leg_hw_init(sparx5, leg);

	dev_dbg(sparx5->dev, "Blackhole leg create vid=%u vmid=%u\n",
		leg->vid, leg->vmid);

	return 0;
}

static void sparx5_rr_blackhole_leg_destroy(struct sparx5 *sparx5)
{
	struct list_head *leg_list = &sparx5->router->leg_list;
	u16 vmid = sparx5_rr_blackhole_vmid(sparx5);
	struct sparx5_rr_router_leg *leg, *tmp;

	list_for_each_entry_safe(leg, tmp, leg_list, leg_list_node) {
		if (leg->vmid == vmid) {
			sparx5_rr_router_leg_destroy(leg);
			break;
		}
	}
}

int sparx5_rr_router_init(struct sparx5 *sparx5)
{
	struct sparx5_router *router;
	int err;

	router = kzalloc(sizeof(*sparx5->router), GFP_KERNEL);
	if (!router)
		return -ENOMEM;

	mutex_init(&router->lock);
	sparx5->router = router;
	router->sparx5 = sparx5;

	router->sparx5_router_owq =
		alloc_ordered_workqueue("sparx5_router_order", 0);
	if (!router->sparx5_router_owq) {
		err = -ENOMEM;
		goto err_alloc_workqueue;
	}

	router->fib_nb.notifier_call = sparx5_rr_fib_event;
	err = register_fib_notifier(&init_net, &router->fib_nb, NULL, NULL);
	if (err)
		goto err_register_fib_notifier;

	router->netevent_nb.notifier_call =
		sparx5_rr_netevent_event;
	err = register_netevent_notifier(&router->netevent_nb);
	if (err)
		goto err_register_netevent_notifier;

	router->inetaddr_nb.notifier_call = sparx5_rr_inetaddr_event;
	err = register_inetaddr_notifier(&router->inetaddr_nb);
	if (err)
		goto err_register_inetaddr_notifier;

	router->inetaddr_valid_nb.notifier_call =
		sparx5_rr_inetaddr_valid_event;
	err = register_inetaddr_validator_notifier(&router->inetaddr_valid_nb);
	if (err)
		goto err_register_inetaddr_valid_notifier;

	router->netdevice_nb.notifier_call = sparx5_rr_netdevice_event;
	err = register_netdevice_notifier(&router->netdevice_nb);
	if (err)
		goto err_register_netdevice_notifier;

	router->inet6addr_valid_nb.notifier_call =
		sparx5_rr_inet6addr_valid_event;
	err = register_inet6addr_validator_notifier(&router->inet6addr_valid_nb);
	if (err)
		goto err_register_inet6addr_valid_notifier;

	router->inet6addr_nb.notifier_call = sparx5_rr_inet6addr_event;
	err = register_inet6addr_notifier(&router->inet6addr_nb);
	if (err)
		goto err_register_inet6addr_notifier;

	err = rhashtable_init(&router->neigh_ht,
			      &sparx5_neigh_ht_params);
	if (err)
		goto err_neigh_ht_init;

	err = rhashtable_init(&router->fib_ht,
			      &sparx5_rr_fib_entry_ht_params);
	if (err)
		goto err_fib_ht_init;

	INIT_LIST_HEAD(&router->leg_list);
	INIT_LIST_HEAD(&router->fib_lpm4_list);
	INIT_LIST_HEAD(&router->fib_lpm6_list);

	atomic_set(&router->legs_count, 0);
	router->link_local.vrule_id = 0;
	router->link_local.vrule_id_valid = false;
	/* VCAP API requires a port net_device, to get a sparx5 reference. */
	router->port_dev = sparx5_port_get_ndev(sparx5);
	if (!router->port_dev) {
		err = -ENXIO;
		goto err_get_port_dev;
	}

	/* Enable L3 UC routing on all ports.
	 * TODO: track ports which are part of some VLAN with RLEG ENA.
	 */
	spx5_wr(~0, sparx5, ANA_L3_L3_UC_ENA);
	if (is_sparx5(sparx5)) {
		spx5_wr(~0, sparx5, ANA_L3_L3_UC_ENA1);
		spx5_wr(~0, sparx5, ANA_L3_L3_UC_ENA2);
	}

	/* Enable routing and global router options */
	spx5_rmw(ANA_L3_ROUTING_CFG_L3_ENA_MODE_SET(1) |
		 ANA_L3_ROUTING_CFG_RT_SMAC_UPDATE_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_CPU_RLEG_IP_HDR_FAIL_REDIR_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_CPU_IP4_OPTIONS_REDIR_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_CPU_IP6_HOPBYHOP_REDIR_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_IP6_HC_REDIR_ENA_SET(1) |
		 ANA_L3_ROUTING_CFG_IP4_TTL_REDIR_ENA_SET(1),
		 ANA_L3_ROUTING_CFG_L3_ENA_MODE |
		 ANA_L3_ROUTING_CFG_RT_SMAC_UPDATE_ENA |
		 ANA_L3_ROUTING_CFG_CPU_RLEG_IP_HDR_FAIL_REDIR_ENA |
		 ANA_L3_ROUTING_CFG_CPU_IP4_OPTIONS_REDIR_ENA |
		 ANA_L3_ROUTING_CFG_CPU_IP6_HOPBYHOP_REDIR_ENA |
		 ANA_L3_ROUTING_CFG_IP6_HC_REDIR_ENA |
		 ANA_L3_ROUTING_CFG_IP4_TTL_REDIR_ENA,
		 sparx5, ANA_L3_ROUTING_CFG);

	/* By default, routing related frame edits are done in REW, but when
	 * combining routing with PTP, ANA_ACL must be configured to change DMAC
	 * to next-hop DMAC in order to allow other information to be stored in
	 * the IFH.
	 *
	 * This enables routing related frame edits independently of VCAP_S2
	 * action ACL_RT_MODE.
	 */
	spx5_rmw(ANA_ACL_VCAP_S2_MISC_CTRL_ACL_RT_SEL_SET(1),
		 ANA_ACL_VCAP_S2_MISC_CTRL_ACL_RT_SEL, sparx5,
		 ANA_ACL_VCAP_S2_MISC_CTRL);

	/* Add reserved leg for blackhole routes. */
	err = sparx5_rr_blackhole_leg_create(sparx5);
	if (err)
		goto err_blackhole_leg;

	return 0;

err_blackhole_leg:
err_get_port_dev:
	rhashtable_destroy(&router->fib_ht);
err_fib_ht_init:
	rhashtable_destroy(&router->neigh_ht);
err_neigh_ht_init:
	unregister_inet6addr_notifier(&router->inet6addr_nb);
err_register_inet6addr_notifier:
	unregister_inet6addr_validator_notifier(&router->inet6addr_valid_nb);
err_register_inet6addr_valid_notifier:
	unregister_netdevice_notifier(&router->netdevice_nb);
err_register_netdevice_notifier:
	unregister_inetaddr_validator_notifier(&router->inetaddr_valid_nb);
err_register_inetaddr_valid_notifier:
	unregister_inetaddr_notifier(&router->inetaddr_nb);
err_register_inetaddr_notifier:
	unregister_netevent_notifier(&router->netevent_nb);
err_register_netevent_notifier:
	unregister_fib_notifier(&init_net, &router->fib_nb);
err_register_fib_notifier:
	destroy_workqueue(router->sparx5_router_owq);
err_alloc_workqueue:
	mutex_destroy(&router->lock);
	kfree(router);

	return err;
}

void sparx5_rr_router_deinit(struct sparx5 *sparx5)
{
	struct sparx5_router *router = sparx5->router;

	sparx5_rr_blackhole_leg_destroy(sparx5);
	rhashtable_destroy(&router->fib_ht);
	rhashtable_destroy(&router->neigh_ht);
	unregister_inet6addr_notifier(&router->inet6addr_nb);
	unregister_inet6addr_validator_notifier(&router->inet6addr_valid_nb);
	unregister_netdevice_notifier(&router->netdevice_nb);
	unregister_inetaddr_validator_notifier(&router->inetaddr_valid_nb);
	unregister_inetaddr_notifier(&router->inetaddr_nb);
	unregister_netevent_notifier(&router->netevent_nb);
	unregister_fib_notifier(&init_net, &router->fib_nb);
	destroy_workqueue(router->sparx5_router_owq);
	mutex_destroy(&router->lock);
	kfree(router);
}
