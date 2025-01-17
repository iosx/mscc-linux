// SPDX-License-Identifier: GPL-2.0

#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/timekeeping.h>
#include <linux/bitops.h>
#include <linux/of.h>
#include <linux/mfd/microchip-dpll.h>
#include <linux/regmap.h>

#include "ptp_private.h"

#define DPLL_MODE_REFSEL(index)			(0x284 + (index) * 0x4)
#define DPLL_MODE_REFSEL_MODE_GET(val)		(val & GENMASK(2, 0))

#define DPLL_TOD_CTRL(index)			(0x2b8 + (index))
#define DPLL_TOD_CTRL_SEM			BIT(4)

#define DPLL_DF_OFFSET(index)			(0x300 + (index) * 0x20)
#define DPLL_TOD_SEC(index)			(0x312 + (index) * 0x20)
#define DPLL_TOD_SEC_SIZE			6
#define DPLL_TOD_NSEC(index)			(0x318 + (index) * 0x20)
#define DPLL_TOD_NSEC_SIZE			6

#define DPLL_OUTPUT_CTRL(index)			(0x4a8 + (index))
#define DPLL_OUTPUT_CTRL_SIZE			1
#define DPLL_OUTPUT_CTRL_SYNTH_SEL_GET(val)	((val & GENMASK(6, 4)) >> 4)
#define DPLL_OUTPUT_CTRL_STOP			BIT(1)
#define DPLL_OUTPUT_CTRL_STOP_HIGH		BIT(2)
#define DPLL_OUTPUT_CTRL_STOP_HZ		BIT(3)

#define DPLL_OUTPUT_PHASE_STEP_CTRL		0x4b8
#define DPLL_OUTPUT_PHASE_STEP_CTRL_SIZE	1
#define DPLL_OUTPUT_PHASE_STEP_CTRL_OP(cmd)	(cmd & GENMASK(1, 0))
#define DPLL_OUTPUT_PAHSE_STEP_CTRL_OP_WRITE	3
#define DPLL_OUTPUT_PHASE_STEP_CTRL_OP_MASK	GENMASK(1, 0)
#define DPLL_OUTPUT_PHASE_STEP_CTRL_TOD_STEP	BIT(3)
#define DPLL_OUTPUT_PHASE_STEP_CTRL_DPLL(index)	((index) << 4)
#define DPLL_OUTPUT_PHASE_STEP_NUMBER		0x4b9
#define DPLL_OUTPUT_PHASE_STEP_NUMBER_SIZE	1
#define DPLL_OUTPUT_PHASE_STEP_MASK		0x4ba
#define DPLL_OUTPUT_PHASE_STEP_MASK_SIZE	2
#define DPLL_OUTPUT_PHASE_STEP_DATA		0x4bc
#define DPLL_OUTPUT_PHASE_STEP_DATA_SIZE	4

#define DPLL_SYNTH_MB_MASK			0x682
#define DPLL_SYNTH_MB_MASK_SIZE			2
#define DPLL_SYNTH_MB_SEM			0x684
#define DPLL_SYNTH_MB_SEM_SIZE			1
#define DPLL_SYNTH_MB_SEM_RD			BIT(1)
#define DPLL_SYNTH_FREQ_BASE			0x686
#define DPLL_SYNTH_FREQ_BASE_SIZE		2
#define DPLL_SYNTH_FREQ_MULT			0x688
#define DPLL_SYNTH_FREQ_MULT_SIZE		4
#define DPLL_SYNTH_FREQ_M			0x68c
#define DPLL_SYNTH_FREQ_M_SIZE			2
#define DPLL_SYNTH_FREQ_N			0x68e
#define DPLL_SYNTH_FREQ_N_SIZE			2

#define DPLL_OUTPUT_MB_MASK			0x702
#define DPLL_OUTPUT_MB_MASK_SIZE		2
#define DPLL_OUTPUT_MB_SEM			0x704
#define DPLL_OUTPUT_MB_SEM_SIZE			1
#define DPLL_OUTPUT_MB_SEM_RD			BIT(1)
#define DPLL_OUTPUT_MB_SEM_WR			BIT(0)
#define DPLL_OUTPUT_MODE			0x705
#define DPLL_OUTPUT_MODE_SIZE			1
#define DPLL_OUTPUT_MODE_SIGNAL_FORMAT(val)	((val) << 4)
#define DPLL_OUTPUT_MODE_SIGNAL_FORMAT_GET(val) ((val & GENMASK(7, 4)) >> 4)
#define DPLL_OUTPUT_MODE_SIGNAL_FORMAT_MASK	GENMASK(7, 4)
#define DPLL_OUTPUT_DIV				0x70c
#define DPLL_OUTPUT_DIV_SIZE			4
#define DPLL_OUTPUT_WIDTH			0x710
#define DPLL_OUTPUT_WIDTH_SIZE			4
#define DPLL_OUTPUT_GPO_EN			0x724
#define DPLL_OUTPUT_GPO_EN_SIZE			1

#define ZL3073X_1PPM_FORMAT		281474976

#define ZL3073X_MAX_DPLLS		2
#define ZL3073X_MAX_OUTPUTS		20

#define READ_SLEEP_US			10
#define READ_TIMEOUT_US			100000000

#define ZL3073X_FW_FILENAME		"zl3073x.mfg"
#define ZL3073X_FW_WHITESPACES_SIZE	3
#define ZL3073X_FW_COMMAND_SIZE		1

#define ZL3073X_P_PIN(pin)		((pin) % 2 == 0)
#define ZL3073X_N_PIN(pin)		(!ZL3073X_P_PIN(pin))

static const struct of_device_id zl3073x_match[] = {
	{ .compatible = "microchip,zl80732-phc" },
	{ .compatible = "microchip,zl30732b-phc" },
	{ }
};
MODULE_DEVICE_TABLE(of, zl3073x_match);

enum zl3073x_mode_t {
	ZL3073X_MODE_NCO			= 0x4,
};

enum zl3073x_tod_ctrl_cmd_t {
	ZL3073X_TOD_CTRL_CMD_WRITE_NEXT_1HZ	= 0x1,
	ZL3073X_TOD_CTRL_CMD_READ		= 0x8,
	ZL3073X_TOD_CTRL_CMD_READ_NEXT_1HZ	= 0x9,
};

enum zl3073x_output_mode_signal_format_t {
	ZL3073X_BOTH_DISABLED			= 0x0,
	ZL3073X_BOTH_ENABLED			= 0x4,
	ZL3073X_P_ENABLE			= 0x5,
	ZL3073X_N_ENABLE			= 0x6,
};

struct zl3073x_dpll {
	struct zl3073x		*zl3073x;
	u8			index;

	struct ptp_clock_info	info;
	struct ptp_clock	*clock;
	struct ptp_pin_desc	pins[ZL3073X_MAX_OUTPUTS];

	u16			perout_mask;
};

struct zl3073x {
	struct device		*dev;
	struct mutex		*lock;
	struct regmap		*regmap;
	struct device		*mfd;

	struct zl3073x_dpll	dpll[ZL3073X_MAX_DPLLS];
};

/* When accessing the registers of the DPLL, it is always required to access
 * first the lower address and then the higher address. The MSB of the data is
 * always stored at the lowest address and LSB is stored at the highest address.
 * This format is different than most setups therefore make sure to swap the
 * bytes before writting and after reading so it can be easier to follow the
 * datasheet.  This function was added for this purpose and it is used inside
 * the read and write functions.
 */
static u8 *zl3073x_swap(u8 *swap, u16 count)
{
	int i;

	for (i = 0; i < count / 2; ++i) {
		u8 tmp = swap[i];
		swap[i] = swap[count - i - 1];
		swap[count - i - 1] = tmp;
	}

	return swap;
}

static int zl3073x_read(struct zl3073x *zl3073x, u16 regaddr, u8 *buf, u16 count)
{
	return regmap_bulk_read(zl3073x->regmap, regaddr, buf, count);
}

static int zl3073x_write(struct zl3073x *zl3073x, u16 regaddr, u8 *buf, u16 count)
{
	return regmap_bulk_write(zl3073x->regmap, regaddr, zl3073x_swap(buf, count), count);
}

static void zl3073x_ptp_timestamp_to_bytearray(const struct timespec64 *ts,
					       u8 *sec, u8 *nsec)
{
	sec[0] = (ts->tv_sec >> 0) & 0xff;
	sec[1] = (ts->tv_sec >> 8) & 0xff;
	sec[2] = (ts->tv_sec >> 16) & 0xff;
	sec[3] = (ts->tv_sec >> 24) & 0xff;
	sec[4] = (ts->tv_sec >> 32) & 0xff;
	sec[5] = (ts->tv_sec >> 40) & 0xff;

	nsec[0] = (ts->tv_nsec >> 0) & 0xff;
	nsec[1] = (ts->tv_nsec >> 8) & 0xff;
	nsec[2] = (ts->tv_nsec >> 16) & 0xff;
	nsec[3] = (ts->tv_nsec >> 24) & 0xff;
	nsec[4] = 0;
	nsec[5] = 0;
}

static void zl3073x_ptp_bytearray_to_timestamp(struct timespec64 *ts,
					       u8 *sec, u8 *nsec)
{
	ts->tv_sec = sec[0];
	for (int i = 1; i < DPLL_TOD_SEC_SIZE; ++i) {
		ts->tv_sec = ts->tv_sec << 8;
		ts->tv_sec |= sec[i];
	}

	ts->tv_nsec = nsec[0];
	for (int i = 1; i < DPLL_TOD_NSEC_SIZE - 2; ++i) {
		ts->tv_nsec = ts->tv_nsec << 8;
		ts->tv_nsec |= nsec[i];
	}

	set_normalized_timespec64(ts, ts->tv_sec, ts->tv_nsec);
}

static int zl3073x_ptp_tod_sem(struct zl3073x_dpll *dpll)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 sem;

	zl3073x_read(zl3073x, DPLL_TOD_CTRL(dpll->index), &sem, sizeof(sem));
	return sem;
}

static int zl3073x_ptp_phase_ctrl_op(struct zl3073x_dpll *dpll)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 ctrl;

	zl3073x_read(zl3073x, DPLL_OUTPUT_PHASE_STEP_CTRL, &ctrl, sizeof(ctrl));
	return ctrl;
}

static int zl3073x_ptp_synth_mb_sem(struct zl3073x_dpll *dpll)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 sem;

	zl3073x_read(zl3073x, DPLL_SYNTH_MB_SEM, &sem, sizeof(sem));
	return sem;
}

static int zl3073x_ptp_output_mb_sem(struct zl3073x_dpll *dpll)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 sem;

	zl3073x_read(zl3073x, DPLL_OUTPUT_MB_SEM, &sem, sizeof(sem));
	return sem;
}

static int _zl3073x_ptp_gettime64(struct zl3073x_dpll *dpll,
				  struct timespec64 *ts,
				  enum zl3073x_tod_ctrl_cmd_t cmd)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 nsec[DPLL_TOD_NSEC_SIZE];
	u8 sec[DPLL_TOD_SEC_SIZE];
	int ret;
	u32 val;
	u8 ctrl;

	/* Check that the semaphore is clear */
	ret = readx_poll_timeout_atomic(zl3073x_ptp_tod_sem, dpll,
					val, !(DPLL_TOD_CTRL_SEM & val),
					READ_SLEEP_US, READ_TIMEOUT_US);
	if (ret)
		return ret;

	/* Issue the read command */
	ctrl = DPLL_TOD_CTRL_SEM | cmd;
	zl3073x_write(zl3073x, DPLL_TOD_CTRL(dpll->index), &ctrl, sizeof(ctrl));

	/* Check that the semaphore is clear */
	ret = readx_poll_timeout_atomic(zl3073x_ptp_tod_sem, dpll,
					val, !(DPLL_TOD_CTRL_SEM & val),
					READ_SLEEP_US, READ_TIMEOUT_US);
	if (ret)
		return ret;;

	/* Read the second and nanoseconds */
	zl3073x_read(zl3073x, DPLL_TOD_SEC(dpll->index),
		     sec, DPLL_TOD_SEC_SIZE);
	zl3073x_read(zl3073x, DPLL_TOD_NSEC(dpll->index),
		     nsec, DPLL_TOD_NSEC_SIZE);

	zl3073x_ptp_bytearray_to_timestamp(ts, sec, nsec);

	return 0;
}

static int zl3073x_ptp_gettime64(struct ptp_clock_info *ptp,
				 struct timespec64 *ts)
{
	struct zl3073x_dpll *dpll = container_of(ptp, struct zl3073x_dpll, info);
	struct zl3073x *zl3073x = dpll->zl3073x;
	int ret;

	mutex_lock(zl3073x->lock);
	ret = _zl3073x_ptp_gettime64(dpll, ts, ZL3073X_TOD_CTRL_CMD_READ);
	mutex_unlock(zl3073x->lock);

	return ret;
}

static int _zl3073x_ptp_settime64(struct zl3073x_dpll *dpll,
				  const struct timespec64 *ts,
				  enum zl3073x_tod_ctrl_cmd_t cmd)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 nsec[DPLL_TOD_NSEC_SIZE];
	u8 sec[DPLL_TOD_SEC_SIZE];
	int ret;
	int val;
	u8 ctrl;

	/* Check that the semaphore is clear */
	ret = readx_poll_timeout_atomic(zl3073x_ptp_tod_sem, dpll,
					val, !(DPLL_TOD_CTRL_SEM & val),
					READ_SLEEP_US, READ_TIMEOUT_US);
	if (ret)
		return ret;

	/* Convert input to something that DPLL can understand */
	zl3073x_ptp_timestamp_to_bytearray(ts, sec, nsec);

	/* Write the value */
	zl3073x_write(zl3073x, DPLL_TOD_SEC(dpll->index),
		      sec, DPLL_TOD_SEC_SIZE);
	zl3073x_write(zl3073x, DPLL_TOD_NSEC(dpll->index),
		      nsec, DPLL_TOD_NSEC_SIZE);

	/* Issue the write command */
	ctrl = DPLL_TOD_CTRL_SEM | cmd;
	zl3073x_write(zl3073x, DPLL_TOD_CTRL(dpll->index), &ctrl, sizeof(ctrl));

	return 0;
}

static int zl3073x_ptp_settime64(struct ptp_clock_info *ptp,
				 const struct timespec64 *ts)
{
	struct zl3073x_dpll *dpll = container_of(ptp, struct zl3073x_dpll, info);
	struct zl3073x *zl3073x = dpll->zl3073x;
	int ret;

	mutex_lock(zl3073x->lock);
	ret = _zl3073x_ptp_settime64(dpll, ts, ZL3073X_TOD_CTRL_CMD_WRITE_NEXT_1HZ);
	mutex_unlock(zl3073x->lock);

	return ret;
}

static int zl3073x_ptp_wait_sec_rollover(struct zl3073x_dpll *dpll)
{
	struct timespec64 init_ts, ts;
	int val;
	int ret;

	memset(&init_ts, 0, sizeof(init_ts));

	do {
		/* Check that the semaphore is clear */
		ret = readx_poll_timeout_atomic(zl3073x_ptp_tod_sem, dpll,
						val, !(DPLL_TOD_CTRL_SEM & val),
						READ_SLEEP_US, READ_TIMEOUT_US);
		if (ret)
			return ret;

		/* Read the time */
		ret = _zl3073x_ptp_gettime64(dpll, &ts,
					     ZL3073X_TOD_CTRL_CMD_READ_NEXT_1HZ);
		if (ret)
			return ret;

		/* Determin if the second has roll over */
		if (!init_ts.tv_sec) {
			init_ts = ts;
		} else {
			if (init_ts.tv_sec < ts.tv_sec)
				break;
		}

		msleep(10);
	} while (true);

	return 0;
}

static s64 _zl3073x_ptp_get_synth_freq(struct zl3073x_dpll *dpll, u8 synth)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u16 numerator;
	u16 denomitor;
	u8 buf[4];
	u16 base;
	u32 mult;
	int ret;
	int val;

	/* Select the synth */
	memset(buf, 0, sizeof(buf));
	buf[0] = BIT(synth);
	zl3073x_write(zl3073x, DPLL_SYNTH_MB_MASK, buf,
		      DPLL_SYNTH_MB_MASK_SIZE);

	/* Select read command */
	memset(buf, 0, sizeof(buf));
	buf[0] = DPLL_SYNTH_MB_SEM_RD;
	zl3073x_write(zl3073x, DPLL_SYNTH_MB_SEM, buf,
		      DPLL_SYNTH_MB_SEM_SIZE);

	/* Wait for the command to actually finish */
	ret = readx_poll_timeout_atomic(zl3073x_ptp_synth_mb_sem, dpll,
					val,
					!(DPLL_SYNTH_MB_SEM_RD & val),
					READ_SLEEP_US, READ_TIMEOUT_US);
	if (ret)
		return ret;

	/* The output frequency is determined by the following formula:
	 * base * multiplier * numerator / denomitor
	 * Therefore get all this number and calculate the output frequency
	 */
	zl3073x_read(zl3073x, DPLL_SYNTH_FREQ_BASE, buf,
		     DPLL_SYNTH_FREQ_BASE_SIZE);
	base = buf[0] << 8;
	base |= buf[1];

	zl3073x_read(zl3073x, DPLL_SYNTH_FREQ_MULT, buf,
		     DPLL_SYNTH_FREQ_MULT_SIZE);
	mult = buf[0] << 24;
	mult |= buf[1] << 16;
	mult |= buf[2] << 8;
	mult |= buf[3];

	zl3073x_read(zl3073x, DPLL_SYNTH_FREQ_M, buf,
		     DPLL_SYNTH_FREQ_M_SIZE);
	numerator = buf[0] << 8;
	numerator |= buf[1];

	zl3073x_read(zl3073x, DPLL_SYNTH_FREQ_N, buf,
		     DPLL_SYNTH_FREQ_N_SIZE);
	denomitor = buf[0] << 8;
	denomitor |= buf[1];

	return base * mult * numerator / denomitor;
}

static int _zl3073x_ptp_adjphase(struct zl3073x_dpll *dpll, const s64 delta)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	s32 register_units;
	u8 buf[4];
	u8 synth;
	int val;
	int ret;

	/* Wait for the previous command to finish */
	ret = readx_poll_timeout_atomic(zl3073x_ptp_phase_ctrl_op, dpll,
					val,
					!(DPLL_OUTPUT_PHASE_STEP_CTRL_OP_MASK & val),
					READ_SLEEP_US, READ_TIMEOUT_US);
	if (ret)
		return ret;

	/* Set the number of steps to take, the value is 1 as we want to finish
	 * fast
	 */
	memset(buf, 0, sizeof(buf));
	buf[0] = 1;
	zl3073x_write(zl3073x, DPLL_OUTPUT_PHASE_STEP_NUMBER, buf,
		      DPLL_OUTPUT_PHASE_STEP_NUMBER_SIZE);

	/* Get the synth that is connected to the output, it is OK to get the
	 * synth for only 1 output as it is expected that all the outputs that
	 * are used by 1PPS are connected to same synth.
	 */
	zl3073x_read(zl3073x, DPLL_OUTPUT_CTRL(__ffs(dpll->perout_mask)), buf,
		     DPLL_OUTPUT_CTRL_SIZE);
	synth = DPLL_OUTPUT_CTRL_SYNTH_SEL_GET(buf[0]);

	/* Configure the step */
	register_units = div_s64(delta * _zl3073x_ptp_get_synth_freq(dpll, synth),
				 NSEC_PER_SEC);

	memset(buf, 0, sizeof(buf));
	buf[0] = register_units & 0xff;
	buf[1] = (register_units >> 8) & 0xff;
	buf[2] = (register_units >> 16) & 0xff;
	buf[3] = (register_units >> 24) & 0xff;
	zl3073x_write(zl3073x, DPLL_OUTPUT_PHASE_STEP_DATA, buf,
		      DPLL_OUTPUT_PHASE_STEP_DATA_SIZE);

	/* Select which output should be adjusted */
	memset(buf, 0, sizeof(buf));
	buf[0] = dpll->perout_mask;
	zl3073x_write(zl3073x, DPLL_OUTPUT_PHASE_STEP_MASK, buf,
		      DPLL_OUTPUT_PHASE_STEP_MASK_SIZE);

	/* Start the phase adjustment on the output pin and also on the ToD */
	memset(buf, 0, sizeof(buf));
	buf[0] = DPLL_OUTPUT_PHASE_STEP_CTRL_DPLL(dpll->index) |
		 DPLL_OUTPUT_PHASE_STEP_CTRL_OP(DPLL_OUTPUT_PAHSE_STEP_CTRL_OP_WRITE) |
		 DPLL_OUTPUT_PHASE_STEP_CTRL_TOD_STEP;

	zl3073x_write(zl3073x, DPLL_OUTPUT_PHASE_STEP_CTRL, buf,
		      DPLL_OUTPUT_PHASE_STEP_CTRL_SIZE);

	return 0;
}

static void zl3073x_ptp_stop_1pps(struct zl3073x_dpll *dpll)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 buf;

	for (size_t i = 0; i < ZL3073X_MAX_OUTPUTS; ++i) {
		if (!(BIT(i) & dpll->perout_mask))
			continue;

		zl3073x_read(zl3073x, DPLL_OUTPUT_CTRL(i), &buf,
			     DPLL_OUTPUT_CTRL_SIZE);
		buf |= DPLL_OUTPUT_CTRL_STOP;

		buf &= ~(DPLL_OUTPUT_CTRL_STOP_HZ &
			 DPLL_OUTPUT_CTRL_STOP_HIGH);

		zl3073x_write(zl3073x, DPLL_OUTPUT_CTRL(i), &buf,
			      DPLL_OUTPUT_CTRL_SIZE);
	}
}

static void zl3073x_ptp_start_1pps(struct zl3073x_dpll *dpll)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 buf;

	for (size_t i = 0; i < ZL3073X_MAX_OUTPUTS; ++i) {
		if (!(BIT(i) & dpll->perout_mask))
			continue;

		zl3073x_read(zl3073x, DPLL_OUTPUT_CTRL(i), &buf,
			     DPLL_OUTPUT_CTRL_SIZE);
		buf &= ~DPLL_OUTPUT_CTRL_STOP;

		zl3073x_write(zl3073x, DPLL_OUTPUT_CTRL(i), &buf,
			      DPLL_OUTPUT_CTRL_SIZE);
	}
}

static int zl3073x_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct zl3073x_dpll *dpll = container_of(ptp, struct zl3073x_dpll, info);
	struct zl3073x *zl3073x = dpll->zl3073x;
	struct timespec64 ts;
	int ret;

	mutex_lock(zl3073x->lock);

	/* When adjusting the time and having a 1PPS enabled then it is the 1PPS
	 * will come more often then 1 per second and this might introduce
	 * issues. For example when running ts2phc then it might see twice the
	 * 1PPS and when it is running on multiple TSU then it migth use the
	 * wrong timestamps. Therefore here disable the 1PPS and just enable it
	 * at later point(in 3 seconds) to make sure the time is adjusted and
	 * the listeners of the 1PPS will not see multiple signals.
	 */
	if (dpll->perout_mask)
		zl3073x_ptp_stop_1pps(dpll);

	if (delta >= NSEC_PER_SEC || delta <= -NSEC_PER_SEC) {
		/* wait for rollover */
		ret = zl3073x_ptp_wait_sec_rollover(dpll);
		if (ret)
			goto out;

		/* get the predicted TOD at the next internal 1PPS */
		ret = _zl3073x_ptp_gettime64(dpll, &ts,
					     ZL3073X_TOD_CTRL_CMD_READ_NEXT_1HZ);
		if (ret)
			goto out;

		ts = timespec64_add(ts, ns_to_timespec64(delta));

		ret = _zl3073x_ptp_settime64(dpll, &ts,
					     ZL3073X_TOD_CTRL_CMD_WRITE_NEXT_1HZ);
		if (ret)
			goto out;
	} else {
		ret = _zl3073x_ptp_adjphase(dpll, delta);
	}

out:
	if (dpll->perout_mask)
		ptp_schedule_worker(dpll->clock, nsecs_to_jiffies(1 * NSEC_PER_SEC));

	mutex_unlock(zl3073x->lock);

	return ret;
}

static int zl3073x_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct zl3073x_dpll *dpll = container_of(ptp, struct zl3073x_dpll, info);
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 dco[6];
	s64 ref;
	s64 ppm;

	/* Store the scaled_ppm into a s64 variable because on 32bit arch, the
	 * multiplication with ZL30373X_1PPM_FORMAT with overflow meaning that
	 * will not be able to adjust to lowest ns
	 */
	ppm = scaled_ppm;
	if (!ppm)
		return 0;

	mutex_lock(zl3073x->lock);

	ref = ZL3073X_1PPM_FORMAT * (ppm >> 16);
	ref += (ZL3073X_1PPM_FORMAT * (0xffff & ppm)) >> 16;

	/* The value that is written in HW is in 2 complement */
	ref = ~ref + 1;

	dco[5] = ref >> 40;
	dco[4] = ref >> 32;
	dco[3] = ref >> 24;
	dco[2] = ref >> 16;
	dco[1] = ref >>  8;
	dco[0] = ref >>  0;

	zl3073x_write(zl3073x, DPLL_DF_OFFSET(dpll->index), dco, sizeof(dco));

	mutex_unlock(zl3073x->lock);

	return 0;
}

static enum zl3073x_output_mode_signal_format_t
_zl3073x_ptp_disable_pin(enum zl3073x_output_mode_signal_format_t current_mode,
			 u8 pin)
{
	switch (current_mode) {
	case ZL3073X_P_ENABLE:
		if (ZL3073X_P_PIN(pin))
			return ZL3073X_BOTH_DISABLED;
		break;
	case ZL3073X_N_ENABLE:
		if (ZL3073X_N_PIN(pin))
			return ZL3073X_BOTH_DISABLED;
		break;
	case ZL3073X_BOTH_ENABLED:
		if (ZL3073X_P_PIN(pin))
			return ZL3073X_N_ENABLE;
		else
			return ZL3073X_P_ENABLE;
	default:
		return ZL3073X_BOTH_DISABLED;
	}

	return ZL3073X_BOTH_DISABLED;
}

static int zl3073x_ptp_perout_disable(struct zl3073x_dpll *dpll,
				      struct ptp_perout_request *perout)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 buf[2];
	int pin;
	int ret;
	int val;
	u8 mode;

	pin = ptp_find_pin(dpll->clock, PTP_PF_PEROUT, perout->index);
	if (pin == -1 || pin >= ZL3073X_MAX_OUTPUTS)
		return -EINVAL;

	/* Select the output pin */
	memset(buf, 0, sizeof(buf));
	buf[0] = BIT(pin / 2);
	zl3073x_write(zl3073x, DPLL_OUTPUT_MB_MASK, buf,
		      DPLL_OUTPUT_MB_MASK_SIZE);

	/* Select read command  */
	memset(buf, 0, sizeof(buf));
	buf[0] = DPLL_OUTPUT_MB_SEM_RD;
	zl3073x_write(zl3073x, DPLL_OUTPUT_MB_SEM, buf,
		      DPLL_OUTPUT_MB_SEM_SIZE);

	/* Wait for the command to actually finish */
	ret = readx_poll_timeout_atomic(zl3073x_ptp_output_mb_sem, dpll,
					val,
					!(DPLL_OUTPUT_MB_SEM_RD & val),
					READ_SLEEP_US, READ_TIMEOUT_US);
	if (ret)
		return ret;

	/* Read current configuration */
	zl3073x_read(zl3073x, DPLL_OUTPUT_MODE, buf,
		     DPLL_OUTPUT_MODE_SIZE);

	mode = DPLL_OUTPUT_MODE_SIGNAL_FORMAT_GET(buf[0]);
	buf[0] &= ~DPLL_OUTPUT_MODE_SIGNAL_FORMAT_MASK;
	buf[0] |= DPLL_OUTPUT_MODE_SIGNAL_FORMAT(_zl3073x_ptp_disable_pin(mode,
									  pin));

	/* Update the configuration */
	zl3073x_write(zl3073x, DPLL_OUTPUT_MODE, buf,
		      DPLL_OUTPUT_MODE_SIZE);

	/* Select write command */
	memset(buf, 0, sizeof(buf));
	buf[0] = DPLL_OUTPUT_MB_SEM_WR;
	zl3073x_write(zl3073x, DPLL_OUTPUT_MB_SEM, buf,
		      DPLL_OUTPUT_MB_SEM_SIZE);

	/* Wait for the command to actually finish */
	ret = readx_poll_timeout_atomic(zl3073x_ptp_output_mb_sem, dpll,
					val,
					!(DPLL_OUTPUT_MB_SEM_WR & val),
					READ_SLEEP_US, READ_TIMEOUT_US);
	if (ret)
		return ret;

	dpll->perout_mask &= ~BIT(pin / 2);

	return 0;
}

static enum zl3073x_output_mode_signal_format_t
_zl3073x_ptp_enable_pin(enum zl3073x_output_mode_signal_format_t current_mode,
			u8 pin)
{
	switch (current_mode) {
	case ZL3073X_P_ENABLE:
		if (ZL3073X_N_PIN(pin))
			return ZL3073X_BOTH_ENABLED;
		break;
	case ZL3073X_N_ENABLE:
		if (ZL3073X_P_PIN(pin))
			return ZL3073X_BOTH_ENABLED;
		break;
	case ZL3073X_BOTH_DISABLED:
		if (ZL3073X_P_PIN(pin))
			return ZL3073X_P_ENABLE;
		else
			return ZL3073X_N_ENABLE;
	default:
		return ZL3073X_BOTH_ENABLED;
	}

	return ZL3073X_BOTH_ENABLED;
}

static int zl3073x_ptp_perout_enable(struct zl3073x_dpll *dpll,
				     struct ptp_perout_request *perout)
{
	struct zl3073x *zl3073x = dpll->zl3073x;
	u8 buf[4];
	u32 width;
	u32 freq;
	u8 synth;
	int pin;
	int ret;
	int val;
	u8 mode;

	pin = ptp_find_pin(dpll->clock, PTP_PF_PEROUT, perout->index);
	if (pin == -1 || pin >= ZL3073X_MAX_OUTPUTS)
		return -EINVAL;

	/* Select the output pin */
	memset(buf, 0, sizeof(buf));
	buf[0] = BIT(pin / 2);
	zl3073x_write(zl3073x, DPLL_OUTPUT_MB_MASK, buf,
		      DPLL_OUTPUT_MB_MASK_SIZE);

	/* Select read command  */
	memset(buf, 0, sizeof(buf));
	buf[0] = DPLL_OUTPUT_MB_SEM_RD;
	zl3073x_write(zl3073x, DPLL_OUTPUT_MB_SEM, buf,
		      DPLL_OUTPUT_MB_SEM_SIZE);

	/* Wait for the command to actually finish */
	ret = readx_poll_timeout_atomic(zl3073x_ptp_output_mb_sem, dpll,
					val,
					!(DPLL_OUTPUT_MB_SEM_RD & val),
					READ_SLEEP_US, READ_TIMEOUT_US);
	if (ret)
		return ret;

	/* Read configuration of output mode */
	zl3073x_read(zl3073x, DPLL_OUTPUT_MODE, buf,
		     DPLL_OUTPUT_MODE_SIZE);

	mode = DPLL_OUTPUT_MODE_SIGNAL_FORMAT_GET(buf[0]);
	buf[0] &= ~DPLL_OUTPUT_MODE_SIGNAL_FORMAT_MASK;
	buf[0] |= DPLL_OUTPUT_MODE_SIGNAL_FORMAT(_zl3073x_ptp_enable_pin(mode,
									 pin));

	/* Update the configuration */
	zl3073x_write(zl3073x, DPLL_OUTPUT_MODE, buf,
		      DPLL_OUTPUT_MODE_SIZE);

	/* Make sure that the output is set as clock and not GPIO */
	buf[0] = 0x0;
	zl3073x_write(zl3073x, DPLL_OUTPUT_GPO_EN, buf, DPLL_OUTPUT_GPO_EN_SIZE);

	/* Get the synth that is connected to the output and set the same value
	 * in the ouput divider of the pin so it can get an 1PPS as this is the
	 * only value supported
	 */
	zl3073x_read(zl3073x, DPLL_OUTPUT_CTRL(pin / 2), buf,
		     DPLL_OUTPUT_CTRL_SIZE);
	synth = DPLL_OUTPUT_CTRL_SYNTH_SEL_GET(buf[0]);
	freq = _zl3073x_ptp_get_synth_freq(dpll, synth);
	memset(buf, 0, sizeof(buf));
	buf[3] = (freq >> 24) & 0xff;
	buf[2] = (freq >> 16) & 0xff;
	buf[1] = (freq >>  8) & 0xff;
	buf[0] = freq & 0xff;
	zl3073x_write(zl3073x, DPLL_OUTPUT_DIV, buf,
		      DPLL_OUTPUT_DIV_SIZE);

	if (perout->flags & PTP_PEROUT_DUTY_CYCLE) {
		if (perout->on.sec)
			return -EINVAL;

		memset(buf, 0, sizeof(buf));

		/* The value that needs to be written in the register is
		 * calculated as following:
		 * width = perout->on.nsec / (NSEC_PER_SEC / freq) * 2
		 * Bellow is just simplify the calculation
		 */
		width = NSEC_PER_SEC / perout->on.nsec;
		width = freq / width;
		width = width * 2;

		buf[3] = (width >> 24) & 0xff;
		buf[2] = (width >> 16) & 0xff;
		buf[1] = (width >>  8) & 0xff;
		buf[0] = width & 0xff;
		zl3073x_write(zl3073x, DPLL_OUTPUT_WIDTH, buf,
			      DPLL_OUTPUT_WIDTH_SIZE);
	}

	/* Select write command */
	memset(buf, 0, sizeof(buf));
	buf[0] = DPLL_OUTPUT_MB_SEM_WR;
	zl3073x_write(zl3073x, DPLL_OUTPUT_MB_SEM, buf,
		      DPLL_OUTPUT_MB_SEM_SIZE);

	/* Wait for the command to actually finish */
	ret = readx_poll_timeout_atomic(zl3073x_ptp_output_mb_sem, dpll,
					val,
					!(DPLL_OUTPUT_MB_SEM_WR & val),
					READ_SLEEP_US, READ_TIMEOUT_US);
	if (ret)
		return ret;

	dpll->perout_mask |= BIT(pin / 2);

	return 0;
}

static int zl3073x_ptp_enable(struct ptp_clock_info *ptp,
			      struct ptp_clock_request *rq, int on)
{
	struct zl3073x_dpll *dpll = container_of(ptp, struct zl3073x_dpll, info);
	struct zl3073x *zl3073x = dpll->zl3073x;
	int err;

	switch (rq->type) {
	case PTP_CLK_REQ_PEROUT:
		mutex_lock(zl3073x->lock);
		if (!on)
			err = zl3073x_ptp_perout_disable(dpll, &rq->perout);
		/* Only accept a 1-PPS aligned to the second. */
		else if (rq->perout.start.nsec || rq->perout.period.sec != 1 ||
			 rq->perout.period.nsec)
			err = -ERANGE;
		else
			err = zl3073x_ptp_perout_enable(dpll, &rq->perout);
		mutex_unlock(zl3073x->lock);
		break;
	default:
		return -1;
	}

	return err;
}

static int zl3073x_ptp_verify(struct ptp_clock_info *ptp, unsigned int pin,
			      enum ptp_pin_function func, unsigned int chan)
{
	switch (func) {
	case PTP_PF_NONE:
	case PTP_PF_PEROUT:
		break;
	default:
		return -1;
	}

	return 0;
}

static long zl3073x_ptp_do_aux_work(struct ptp_clock_info *ptp)
{
	struct zl3073x_dpll *dpll = container_of(ptp, struct zl3073x_dpll, info);

	if (dpll->perout_mask)
		zl3073x_ptp_start_1pps(dpll);

	return -1;
}

static struct ptp_clock_info zl3073x_ptp_clock_info = {
	.owner		= THIS_MODULE,
	.name		= "zl3073x ptp",
	.max_adj	= 1000000000,
	.gettime64	= zl3073x_ptp_gettime64,
	.settime64	= zl3073x_ptp_settime64,
	.adjtime	= zl3073x_ptp_adjtime,
	.adjfine	= zl3073x_ptp_adjfine,
	.enable		= zl3073x_ptp_enable,
	.verify		= zl3073x_ptp_verify,
	.do_aux_work	= zl3073x_ptp_do_aux_work,
	.n_per_out	= ZL3073X_MAX_OUTPUTS,
	.n_ext_ts	= ZL3073X_MAX_OUTPUTS,
	.n_pins		= ZL3073X_MAX_OUTPUTS,
};

static int zl3073x_ptp_init(struct zl3073x *zl3073x, u8 index)
{
	struct zl3073x_dpll *dpll = &zl3073x->dpll[index];

	for (int i = 0; i < ZL3073X_MAX_OUTPUTS; i++) {
		struct ptp_pin_desc *p = &dpll->pins[i];

		snprintf(p->name, sizeof(p->name), "pin%d", i);
		p->index = i;
		p->func = PTP_PF_NONE;
		p->chan = 0;
	}

	dpll->index = index;
	dpll->zl3073x = zl3073x;
	dpll->info = zl3073x_ptp_clock_info;
	dpll->info.pin_config = dpll->pins;
	dpll->clock = ptp_clock_register(&dpll->info, zl3073x->dev);
	if (IS_ERR(dpll->clock))
		return PTR_ERR(dpll->clock);

	return 0;
}

static const char *_zl3073x_firmware_get_line(const char *data,
					      size_t line_number)
{
	for (int i = 0; i < line_number; ++i) {
		data = strchr(data, '\n');
		if (!data)
			return NULL;
		data += 1;
	}

	return data;
}

static int _zl3073x_firmware_parse_line(struct zl3073x *zl3073x,
					const char *line)
{
	const char *tmp = line;
	int err = 0;
	u8 val = 0;
	char *endp;
	u32 delay;
	u16 addr;

	mutex_lock(zl3073x->lock);
	switch (tmp[0]) {
	case 'X':
		/* The line looks like this:
		 * X , ADDR , VAL
		 * Where:
		 *  - X means that is a command that needs to be executed
		 *  - ADDR represents the addr and is always 2 bytes and the
		 *         value is in hex, for example 0x0232
		 *  - VAL represents the value that is written and is always 1
		 *        byte and the value is in hex, for example 0x12
		 */
		tmp += ZL3073X_FW_COMMAND_SIZE;

		tmp += ZL3073X_FW_WHITESPACES_SIZE;
		addr = simple_strtoul(tmp, &endp, 16);

		tmp = endp;
		tmp += ZL3073X_FW_WHITESPACES_SIZE;
		val = simple_strtoul(tmp, &endp, 16);

		err = zl3073x_write(zl3073x, addr, &val, 1);
		break;
	case 'W':
		/* The line looks like this:
		 * W , DELAY
		 * Where:
		 *  - W means that is a wait command
		 *  - DELAY represents the delay in microseconds and the value
		 *    is in decimal
		 */
		tmp += ZL3073X_FW_COMMAND_SIZE;

		tmp += ZL3073X_FW_WHITESPACES_SIZE;
		delay = simple_strtoul(tmp, &endp, 10);

		usleep_range(delay / 2, delay);
		break;
	default:
		break;
	}
	mutex_unlock(zl3073x->lock);

	return err;
}

static int zl3073x_firmware_load(struct zl3073x *zl3073x)
{
	char fname[128] = ZL3073X_FW_FILENAME;
	const struct firmware *fw;
	size_t line_number = 0;
	const char *line;
	int err = 0;

	err = request_firmware(&fw, fname, zl3073x->dev);
	if (err)
		return err;

	while (true) {
		line = _zl3073x_firmware_get_line(fw->data, line_number);
		if (!line)
			goto out;

		line_number += 1;

		/* Skip comment lines */
		if (line[0] == ';')
			continue;

		err = _zl3073x_firmware_parse_line(zl3073x, line);
		if (err)
			goto out;
	}

out:
	release_firmware(fw);
	return err;
}

static bool zl3073x_dpll_nco_mode(struct zl3073x *zl3073x, int dpll_index)
{
	u8 mode;

	zl3073x_read(zl3073x, DPLL_MODE_REFSEL(dpll_index), &mode, sizeof(mode));
	return DPLL_MODE_REFSEL_MODE_GET(mode) == ZL3073X_MODE_NCO;
}

static int zl3073x_probe(struct platform_device *pdev)
{
	struct microchip_dpll_ddata *ddata = dev_get_drvdata(pdev->dev.parent);
	struct zl3073x *zl3073x;
	int err;

	zl3073x = devm_kzalloc(&pdev->dev, sizeof(struct zl3073x), GFP_KERNEL);
	if (!zl3073x)
		return -ENOMEM;

	zl3073x->dev = &pdev->dev;
	zl3073x->mfd = pdev->dev.parent;
	zl3073x->lock = &ddata->lock;
	zl3073x->regmap = ddata->regmap;

	zl3073x_firmware_load(zl3073x);

	for (size_t i = 0; i < ZL3073X_MAX_DPLLS; ++i) {
		if (!zl3073x_dpll_nco_mode(zl3073x, i))
			continue;

		err = zl3073x_ptp_init(zl3073x, i);
		if (err)
			return err;
	}

	platform_set_drvdata(pdev, zl3073x);

	return 0;
}

static int zl3073x_remove(struct platform_device *pdev)
{
	struct zl3073x *zl3073x = platform_get_drvdata(pdev);

	for (int i = 0; i < ZL3073X_MAX_DPLLS; ++i) {
		if (!zl3073x_dpll_nco_mode(zl3073x, i))
			continue;

		ptp_clock_unregister(zl3073x->dpll[i].clock);
	}

	return 0;
}

static struct platform_driver zl3073x_driver = {
	.driver = {
		.name = "microchip,zl3073x-phc",
		.of_match_table = zl3073x_match,
	},
	.probe = zl3073x_probe,
	.remove	= zl3073x_remove,
};

module_platform_driver(zl3073x_driver);

MODULE_DESCRIPTION("Driver for zl3073x clock devices");
MODULE_LICENSE("GPL");
