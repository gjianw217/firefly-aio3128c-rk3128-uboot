/*
 *  Copyright (C) 2016 Fuzhou Rockchip Electronics Co., Ltd
 *  ChenJianhong <chenjh@rock-chips.com>
 *  for battery driver sample
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */
#include <errno.h>
#include <common.h>
#include <malloc.h>
#include <fdtdec.h>
#include <power/battery.h>
#include <power/rk816_pmic.h>
#include <power/rockchip_power.h>
#include <asm/arch/rkplat.h>

DECLARE_GLOBAL_DATA_PTR;

static int dbg_enable = 0;
#define DBG(args...) \
	do { \
		if (dbg_enable) { \
			printf(args); \
		} \
	} while (0)

#define BAT_INFO(fmt, args...)	printf("rk816-bat: "fmt, ##args)
#define DRIVER_VERSION		"1.1"

/* THERMAL_REG */
#define TEMP_115C		(0x03 << 2)
#define FB_TEMP_MSK		0x0c

/* CHRG_CTRL_REG2*/
#define FINISH_100MA		(0x00 << 6)
#define FINISH_150MA		(0x01 << 6)
#define FINISH_200MA		(0x02 << 6)
#define FINISH_250MA		(0x03 << 6)
#define FINISH_CUR_MSK		0xc7

/* CHRG_CTRL_REG3*/
#define CHRG_TERM_DIG_SIGNAL	(1 << 5)
#define CHRG_TIMER_CCCV_EN	(1 << 2)

/* CHRG_CTRL_REG */
#define ILIM_450MA		(0x00)
#define CHRG_CT_EN		(1 << 7)

/* USB_CTRL_REG */
#define INPUT_CUR_MSK		0x0f

/* VB_MON_REG */
#define PLUG_IN_STS		(1 << 6)

/* GGSTS */
#define BAT_CON			(1 << 4)
#define VOL_INSTANT		(1 << 0)
#define VOL_AVG			(0 << 0)

/* TS_CTRL_REG */
#define GG_EN			(1 << 7)

/* CHRG_USB_CTRL*/
#define CHRG_EN			(1 << 7)

/*SUP_STS_REG*/
#define BAT_EXS			(1 << 7)
#define USB_EXIST		(1 << 1)
#define USB_EFF			(1 << 0)
#define CHARGE_OFF		(0x00 << 4)
#define DEAD_CHARGE		(0x01 << 4)
#define TRICKLE_CHARGE		(0x02 << 4)
#define CC_OR_CV		(0x03 << 4)
#define CHARGE_FINISH		(0x04 << 4)
#define USB_OVER_VOL		(0x05 << 4)
#define BAT_TMP_ERR		(0x06 << 4)
#define TIMER_ERR		(0x07 << 4)
#define USB_VLIMIT_EN		(1 << 3)
#define USB_CLIMIT_EN		(1 << 2)
#define BAT_STATUS_MSK		0x70

/* GGCON */
#define ADC_CUR_MODE		(1 << 1)

/* CALI PARAM */
#define FINISH_CALI_CURR	1500
#define TERM_CALI_CURR		600
#define	VIRTUAL_POWER_VOL	4200
#define	VIRTUAL_POWER_SOC	66
#define SECONDS(n)		((n) * 1000)

/* CALC PARAM */
#define MAX_PERCENTAGE		100
#define MAX_INTERPOLATE		1000
#define MAX_INT			0x7fff
#define MIN_FCC			500

/* DC ADC */
#define FG_INIT			(1 << 3)
#define FG_RESET_LATE		(1 << 1)
#define FG_RESET_NOW		(1 << 0)
#define CHRG_TERM_DSOC		90
#define CHRG_TERM_K		650
#define CHRG_FULL_K		400
#define ADC_CALIB_THRESHOLD	4
#define DC_ADC_TRIGGER		150
#define SARADC_BASE             RKIO_SARADC_PHYS
#define DIV(x)			((x) ? (x) : 1)

/***********************************************************/
struct battery_info {
	int		state_of_chrg;
	int		poffset;
	int		bat_res;
	int		current_avg;
	int		voltage_avg;
	int		voltage_ocv;
	int		voltage_k;
	int		voltage_b;
	int		dsoc;
	int		rsoc;
	int		fcc;
	int		qmax;
	int		remain_cap;
	int		design_cap;
	int		nac;
	u32		*ocv_table;
	u32		ocv_size;
	int		virtual_power;
	int		pwroff_min;
	int		sm_old_cap;
	int		sm_linek;
	int		sm_chrg_dsoc;
	int		adc_allow_update;
	int		chrg_vol_sel;
	int		chrg_cur_input;
	int		chrg_cur_sel;
	int		dts_vol_sel;
	int		dts_cur_input;
	int		dts_cur_sel;
	int		max_soc_offset;
	struct fdt_gpio_state dc_det;
	int		dc_type;
	int		dc_det_adc;
	ulong		vol_mode_base;
	ulong		finish_chrg_base;
	u8		calc_dsoc;
	u8		calc_rsoc;
	int		sm_meet_soc;
	u8		halt_cnt;
	bool		is_halt;
	bool		is_ocv_calib;
	bool		is_max_soc_offset;
	bool		is_first_power_on;
	bool		is_sw_reset;
	int		pwr_dsoc;
	int		pwr_rsoc;
	int		pwr_vol;
};

struct saradc {
	uint32	index;
	uint32	data;
	uint32	stas;
	uint32	ctrl;
};

enum charger_type {
	NO_CHARGER = 0,
	USB_CHARGER,
	AC_CHARGER,
	DC_CHARGER,
	UNDEF_CHARGER,
};

enum dc_type {
	DC_TYPE_OF_NONE = 0,
	DC_TYPE_OF_GPIO,
	DC_TYPE_OF_ADC,
};


static const u32 CHRG_VOL_SEL[] = {
	4050, 4100, 4150, 4200, 4250, 4300, 4350
};

static const u32 CHRG_CUR_SEL[] = {
	1000, 1200, 1400, 1600, 1800, 2000, 2250, 2400
};

static const u32 CHRG_CUR_INPUT[] = {
	450, 800, 850, 1000, 1250, 1500, 1750, 2000
};

struct rk816_fg {
	struct pmic *p;
	struct battery_info di;
	struct saradc adc;
};

struct rk816_fg rk816_fg;
/**************************************************************/
static inline void write_reg32(unsigned long addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint32_t read_reg32(unsigned long addr)
{
	return *(volatile uint32_t *)addr;
}

static int saradc_init(void)
{
	uint32 value, timeout = 0;

	write_reg32(rk816_fg.adc.ctrl, 0);
	udelay(1);
	write_reg32(rk816_fg.adc.ctrl, 0x0028 | (rk816_fg.adc.index));
	udelay(1);
	do {
		value = read_reg32(rk816_fg.adc.ctrl);
		timeout++;
	} while ((value & 0x40) == 0);

	value = read_reg32(rk816_fg.adc.data);

	return value;
}

static int saradc_get_value(void)
{
	int value;

	saradc_init();
	do {
		value = read_reg32(rk816_fg.adc.ctrl);
	} while ((value & 0x40) == 0);

	value = read_reg32(rk816_fg.adc.data);
	DBG("<%s>. adc=%d\n", __func__, value);

	return value;
}

static int div(int val)
{
	return (val == 0) ? 1 : val;
}

static int rk816_bat_read(u8 reg)
{
	return i2c_reg_read(rk816_fg.p->hw.i2c.addr, reg);
}

static void rk816_bat_write(u8 reg, u8 buf)
{
	i2c_reg_write(rk816_fg.p->hw.i2c.addr, reg, buf);
}

static int rk816_bat_get_rsoc(struct battery_info *di)
{
	return (di->remain_cap + di->fcc/200) * 100 / div(di->fcc);
}

static int rk816_bat_get_dsoc(struct  battery_info *di)
{
	return rk816_bat_read(RK816_SOC_REG);
}

static void rk816_bat_set_vol_instant_mode(struct battery_info *di)
{
	u8 val;

	val = rk816_bat_read(RK816_GGSTS_REG);
	val |= VOL_INSTANT;
	rk816_bat_write(RK816_GGSTS_REG, val);
}

static void rk816_bat_set_vol_avg_mode(struct battery_info *di)
{
	u8 val;

	val = rk816_bat_read(RK816_GGSTS_REG);
	val &= ~0x01;
	val |= VOL_AVG;
	rk816_bat_write(RK816_GGSTS_REG, val);
}

static void rk816_bat_enable_gauge(struct battery_info *di)
{
	u8 val;

	val = rk816_bat_read(RK816_TS_CTRL_REG);
	val |= GG_EN;
	rk816_bat_write(RK816_TS_CTRL_REG, val);
}

static int rk816_bat_get_vcalib0(struct battery_info *di)
{
	int val = 0;

	val |= rk816_bat_read(RK816_VCALIB0_REGL) << 0;
	val |= rk816_bat_read(RK816_VCALIB0_REGH) << 8;

	return val;
}

static int rk816_bat_get_vcalib1(struct battery_info *di)
{
	int val = 0;

	val |= rk816_bat_read(RK816_VCALIB1_REGL) << 0;
	val |= rk816_bat_read(RK816_VCALIB1_REGH) << 8;

	DBG("<%s>. coffset: 0x%x\n", __func__, val);
	return val;
}

static void rk816_bat_set_coffset(struct battery_info *di, int val)
{
	u8 buf;

	buf = (val >> 0) & 0xff;
	rk816_bat_write(RK816_CAL_OFFSET_REGL, buf);
	buf = (val >> 8) & 0xff;
	rk816_bat_write(RK816_CAL_OFFSET_REGH, buf);

	DBG("<%s>. set coffset: 0x%x\n", __func__, val);
}


static int rk816_bat_get_ioffset(struct battery_info *di)
{
	int val = 0;

	val |= rk816_bat_read(RK816_IOFFSET_REGL) << 0;
	val |= rk816_bat_read(RK816_IOFFSET_REGH) << 8;

	return val;
}

static void rk816_bat_init_voltage_kb(struct battery_info *di)
{
	int vcalib0, vcalib1;

	vcalib0 = rk816_bat_get_vcalib0(di);
	vcalib1 = rk816_bat_get_vcalib1(di);
	di->voltage_k = (4200 - 3000) * 1000 / DIV(vcalib1 - vcalib0);
	di->voltage_b = 4200 - (di->voltage_k * vcalib1) / 1000;
	DBG("%s. vk=%d, vb=%d\n", __func__, di->voltage_k, di->voltage_b);
}

static int rk816_bat_get_ocv_voltage(struct battery_info *di)
{
	int vol, val = 0;

	val |= rk816_bat_read(RK816_BAT_OCV_REGL) << 0;
	val |= rk816_bat_read(RK816_BAT_OCV_REGH) << 8;
	vol = di->voltage_k * val / 1000 + di->voltage_b;
	vol = vol * 1100 / 1000;

	return vol;
}

static int rk816_bat_get_avg_current(struct battery_info *di)
{
	int val = 0;

	val |= rk816_bat_read(RK816_BAT_CUR_AVG_REGL) << 0;
	val |= rk816_bat_read(RK816_BAT_CUR_AVG_REGH) << 8;

	if (val & 0x800)
		val -= 4096;
	val = val * 1506 / 1000;

	return val;
}

static int rk816_bat_get_avg_voltage(struct battery_info *di)
{
	int vol, val = 0;

	val |= rk816_bat_read(RK816_BAT_VOL_REGL) << 0;
	val |= rk816_bat_read(RK816_BAT_VOL_REGH) << 8;
	vol = di->voltage_k * val / 1000 + di->voltage_b;
	vol = vol * 1100 / 1000;

	return vol;
}

static int rk816_bat_get_est_voltage(struct battery_info *di)
{
	int est_vol, vol, curr;

	vol = rk816_bat_get_avg_voltage(di);
	curr = rk816_bat_get_avg_current(di);
	est_vol = vol - (di->bat_res * curr / 1000);

	return (est_vol > 2800) ? est_vol : vol;
}

static u8 rk816_bat_finish_ma(int fcc)
{
	u8 ma;

	if (fcc > 5000)
		ma = FINISH_250MA;
	else if (fcc >= 4000)
		ma = FINISH_200MA;
	else if (fcc >= 3000)
		ma = FINISH_150MA;
	else
		ma = FINISH_100MA;

	return ma;
}

static void rk816_bat_select_chrg_cv(struct battery_info *di)
{
	int index, chrg_vol_sel, chrg_cur_sel, chrg_cur_input;

	chrg_vol_sel = di->dts_vol_sel;
	chrg_cur_sel = di->dts_cur_sel;
	chrg_cur_input = di->dts_cur_input;

	for (index = 0; index < ARRAY_SIZE(CHRG_VOL_SEL); index++) {
		if (chrg_vol_sel < CHRG_VOL_SEL[index])
			break;
		di->chrg_vol_sel = (index << 4);
	}

	for (index = 0; index < ARRAY_SIZE(CHRG_CUR_INPUT); index++) {
		if (chrg_cur_input < CHRG_CUR_INPUT[index])
			break;
		di->chrg_cur_input = (index << 0);
	}

	for (index = 0; index < ARRAY_SIZE(CHRG_CUR_SEL); index++) {
		if (chrg_cur_sel < CHRG_CUR_SEL[index])
			break;
		di->chrg_cur_sel = (index << 0);
	}

	DBG("<%s>. vol=0x%x, input=0x%x, sel=0x%x\n",
	    __func__, di->chrg_vol_sel, di->chrg_cur_input, di->chrg_cur_sel);
}

static void rk816_bat_init_chrg_config(struct battery_info *di)
{
	u8 chrg_ctrl1, usb_ctrl, chrg_ctrl2, chrg_ctrl3;
	u8 sup_sts, ggcon, thermal, finish_ma;

	rk816_bat_select_chrg_cv(di);
	finish_ma = rk816_bat_finish_ma(di->fcc);

	ggcon = rk816_bat_read(RK816_GGCON_REG);
	sup_sts = rk816_bat_read(RK816_SUP_STS_REG);
	usb_ctrl = rk816_bat_read(RK816_USB_CTRL_REG);
	thermal = rk816_bat_read(RK816_THERMAL_REG);
	chrg_ctrl2 = rk816_bat_read(RK816_CHRG_CTRL_REG2);
	chrg_ctrl3 = rk816_bat_read(RK816_CHRG_CTRL_REG3);

	/* set charge current and voltage */
	usb_ctrl &= ~INPUT_CUR_MSK;
	usb_ctrl |= di->chrg_cur_input;
	chrg_ctrl1 = (CHRG_EN | di->chrg_vol_sel | di->chrg_cur_sel);

	/* digital signal and finish current*/
	chrg_ctrl3 |= CHRG_TERM_DIG_SIGNAL;
	chrg_ctrl2 &= ~FINISH_CUR_MSK;
	chrg_ctrl2 |= finish_ma;

	/* cccv mode */
	chrg_ctrl3 &= ~CHRG_TIMER_CCCV_EN;

	/* enable voltage limit and enable input current limit */
	sup_sts |= USB_VLIMIT_EN;
	sup_sts |= USB_CLIMIT_EN;

	/* set feedback temperature */
	usb_ctrl |= CHRG_CT_EN;
	thermal &= ~FB_TEMP_MSK;
	thermal |= TEMP_115C;

	/* adc current mode */
	ggcon |= ADC_CUR_MODE;

	rk816_bat_write(RK816_GGCON_REG, ggcon);
	rk816_bat_write(RK816_SUP_STS_REG, sup_sts);
	rk816_bat_write(RK816_USB_CTRL_REG, usb_ctrl);
	rk816_bat_write(RK816_THERMAL_REG, thermal);
	rk816_bat_write(RK816_CHRG_CTRL_REG1, chrg_ctrl1);
	rk816_bat_write(RK816_CHRG_CTRL_REG2, chrg_ctrl2);
	rk816_bat_write(RK816_CHRG_CTRL_REG3, chrg_ctrl3);
}

static u32 interpolate(int value, u32 *table, int size)
{
	uint8_t i;
	uint16_t d;

	for (i = 0; i < size; i++) {
		if (value < table[i])
			break;
	}

	if ((i > 0) && (i < size)) {
		d = (value - table[i-1]) * (MAX_INTERPOLATE/(size-1));
		d /= table[i] - table[i-1];
		d = d + (i-1) * (MAX_INTERPOLATE/(size-1));
	} else {
		d = i * ((MAX_INTERPOLATE+size/2)/size);
	}

	if (d > 1000)
		d = 1000;

	return d;
}

/* returns (a * b) / c */
static int32_t ab_div_c(u32 a, u32 b, u32 c)
{
	bool sign;
	u32 ans = MAX_INT;
	int32_t tmp;

	sign = ((((a^b)^c) & 0x80000000) != 0);

	if (c != 0) {
		if (sign)
			c = -c;

		tmp = ((int32_t) a*b + (c>>1)) / c;

		if (tmp < MAX_INT)
			ans = tmp;
	}

	if (sign)
		ans = -ans;

	return ans;
}

static int rk816_bat_vol_to_cap(struct battery_info *di, int voltage)
{
	u32 *ocv_table, tmp;
	int ocv_size, ocv_cap;

	ocv_table = di->ocv_table;
	ocv_size = di->ocv_size;
	tmp = interpolate(voltage, ocv_table, ocv_size);
	ocv_cap = ab_div_c(tmp, di->fcc, MAX_INTERPOLATE);

	return ocv_cap;
}

static int rk816_bat_vol_to_soc(struct battery_info *di, int voltage)
{
	u32 *ocv_table, tmp;
	int ocv_size, ocv_soc;

	ocv_table = di->ocv_table;
	ocv_size = di->ocv_size;
	tmp = interpolate(voltage, ocv_table, ocv_size);
	ocv_soc = ab_div_c(tmp, MAX_PERCENTAGE, MAX_INTERPOLATE);

	return ocv_soc;
}

static int rk816_bat_get_prev_cap(struct battery_info *di)
{
	int val = 0;

	val |= rk816_bat_read(RK816_REMAIN_CAP_REG3) << 24;
	val |= rk816_bat_read(RK816_REMAIN_CAP_REG2) << 16;
	val |= rk816_bat_read(RK816_REMAIN_CAP_REG1) << 8;
	val |= rk816_bat_read(RK816_REMAIN_CAP_REG0) << 0;

	return val;
}

static void rk816_bat_save_fcc(struct battery_info *di, u32 cap)
{
	u8 buf;

	buf = (cap >> 24) & 0xff;
	rk816_bat_write(RK816_NEW_FCC_REG3, buf);
	buf = (cap >> 16) & 0xff;
	rk816_bat_write(RK816_NEW_FCC_REG2, buf);
	buf = (cap >> 8) & 0xff;
	rk816_bat_write(RK816_NEW_FCC_REG1, buf);
	buf = (cap >> 0) & 0xff;
	rk816_bat_write(RK816_NEW_FCC_REG0, buf);
}

static int rk816_bat_get_fcc(struct battery_info *di)
{
	int val = 0;

	val |= rk816_bat_read(RK816_NEW_FCC_REG3) << 24;
	val |= rk816_bat_read(RK816_NEW_FCC_REG2) << 16;
	val |= rk816_bat_read(RK816_NEW_FCC_REG1) << 8;
	val |= rk816_bat_read(RK816_NEW_FCC_REG0) << 0;

	if (val < MIN_FCC)
		val = di->design_cap;
	else if (val > di->qmax)
		val = di->qmax;

	return val;
}

static u8 rk816_bat_get_pwroff_min(struct battery_info *di)
{
	u8 cur, last;

	cur = rk816_bat_read(RK816_NON_ACT_TIMER_CNT_REG);
	last = rk816_bat_read(RK816_NON_ACT_TIMER_CNT_SAVE_REG);
	rk816_bat_write(RK816_NON_ACT_TIMER_CNT_SAVE_REG, cur);

	return (cur != last) ? cur : 0;
}

static int rk816_bat_get_coulomb_cap(struct battery_info *di)
{
	int val = 0;

	val |= rk816_bat_read(RK816_GASCNT_REG3) << 24;
	val |= rk816_bat_read(RK816_GASCNT_REG2) << 16;
	val |= rk816_bat_read(RK816_GASCNT_REG1) << 8;
	val |= rk816_bat_read(RK816_GASCNT_REG0) << 0;
	val /= 2390;

	return val;
}

static void rk816_bat_init_capacity(struct battery_info *di, u32 capacity)
{
	u8 buf;
	u32 cap;
	int delta;

	delta = capacity - di->remain_cap;
	if (!delta)
		return;

	cap = capacity * 2390;
	buf = (cap >> 24) & 0xff;
	rk816_bat_write(RK816_GASCNT_CAL_REG3, buf);
	buf = (cap >> 16) & 0xff;
	rk816_bat_write(RK816_GASCNT_CAL_REG2, buf);
	buf = (cap >> 8) & 0xff;
	rk816_bat_write(RK816_GASCNT_CAL_REG1, buf);
	buf = (cap >> 0) & 0xff;
	rk816_bat_write(RK816_GASCNT_CAL_REG0, buf);

	di->remain_cap = rk816_bat_get_coulomb_cap(di);
	di->rsoc = rk816_bat_get_rsoc(di);
}

static bool is_rk816_bat_ocv_valid(struct battery_info *di)
{
	return di->pwroff_min >= 30 ? true : false;
}

static int rk816_bat_get_usb_state(struct battery_info *di)
{
	int charger_type;

	switch (dwc_otg_check_dpdm()) {
	case 0:
		if ((rk816_bat_read(RK816_VB_MON_REG) & PLUG_IN_STS) != 0)
			charger_type = DC_CHARGER;
		else
			charger_type = NO_CHARGER;
		break;
	case 1:
	case 3:
		charger_type = USB_CHARGER;
		break;
	case 2:
		charger_type = AC_CHARGER;
		break;
	default:
		charger_type = NO_CHARGER;
	}

	return charger_type;
}

static void rk816_bat_clr_initialized_state(struct battery_info *di)
{
	u8 val;

	val = rk816_bat_read(RK816_MISC_MARK_REG);
	val &= ~FG_INIT;
	rk816_bat_write(RK816_MISC_MARK_REG, val);
}

static bool rk816_bat_is_initialized(struct battery_info *di)
{
	return (rk816_bat_read(RK816_MISC_MARK_REG) & FG_INIT) ? true : false;
}

static void rk816_bat_set_initialized_state(struct battery_info *di)
{
	u8 val;

	val = rk816_bat_read(RK816_MISC_MARK_REG);
	if (rk816_bat_get_usb_state(di) != NO_CHARGER) {
		val |= FG_INIT;
		rk816_bat_write(RK816_MISC_MARK_REG, val);
		BAT_INFO("fuel gauge initialized... estv=%d, ch=%d\n",
			 rk816_bat_get_est_voltage(di),
			 rk816_bat_get_usb_state(di));
	}

}

static void rk816_bat_first_pwron(struct battery_info *di)
{
	int ocv_vol;

	rk816_bat_save_fcc(di, di->design_cap);
	ocv_vol = rk816_bat_get_ocv_voltage(di);
	di->fcc = rk816_bat_get_fcc(di);
	di->nac = rk816_bat_vol_to_cap(di, ocv_vol);
	di->rsoc = rk816_bat_vol_to_soc(di, ocv_vol);
	di->dsoc = di->rsoc;
	rk816_bat_init_capacity(di, di->nac);
	rk816_bat_set_initialized_state(di);

	BAT_INFO("first power on: soc=%d\n", di->dsoc);
}

static u8 rk816_bat_get_halt_cnt(struct battery_info *di)
{
	return rk816_bat_read(RK816_HALT_CNT_REG);
}

static void rk816_bat_inc_halt_cnt(struct battery_info *di)
{
	u8 cnt;

	cnt = rk816_bat_read(RK816_HALT_CNT_REG);
	rk816_bat_write( RK816_HALT_CNT_REG, ++cnt);
}

static bool is_rk816_bat_last_halt(struct battery_info *di)
{
	int pre_cap = rk816_bat_get_prev_cap(di);
	int now_cap = rk816_bat_get_coulomb_cap(di);

	/* over 5%: system halt last time */
	if (abs(now_cap - pre_cap) > (di->fcc / 20)) {
		rk816_bat_inc_halt_cnt(di);
		return true;
	} else {
		return false;
	}
}

static void rk816_bat_not_first_pwron(struct battery_info *di)
{
	int pre_soc, pre_cap, ocv_cap, ocv_soc, ocv_vol, now_cap;

	di->fcc = rk816_bat_get_fcc(di);
	pre_soc = rk816_bat_get_dsoc(di);
	pre_cap = rk816_bat_get_prev_cap(di);
	now_cap = rk816_bat_get_coulomb_cap(di);
	di->pwr_dsoc = pre_soc;
	di->pwr_rsoc = (now_cap + di->fcc / 200) * 100 / DIV(di->fcc);
	di->is_halt = is_rk816_bat_last_halt(di);
	di->halt_cnt = rk816_bat_get_halt_cnt(di);
	di->is_ocv_calib = is_rk816_bat_ocv_valid(di);

	if (di->is_halt) {
		BAT_INFO("system halt last time... cap: pre=%d, now=%d\n",
			 pre_cap, now_cap);
		if (now_cap < 0)
			now_cap = 0;
		rk816_bat_init_capacity(di, now_cap);
		pre_cap = di->remain_cap;
		pre_soc = di->rsoc;
		goto finish;
	} else if (di->is_ocv_calib) {
		ocv_vol = rk816_bat_get_ocv_voltage(di);
		ocv_soc = rk816_bat_vol_to_soc(di, ocv_vol);
		ocv_cap = rk816_bat_vol_to_cap(di, ocv_vol);
		pre_cap = ocv_cap;
		BAT_INFO("do ocv calib.. rsoc=%d\n", ocv_soc);

		if (abs(ocv_soc - pre_soc) >= di->max_soc_offset) {
			BAT_INFO("trigger max soc offset, soc: %d -> %d\n",
				 pre_soc, ocv_soc);
			pre_soc = ocv_soc;
			di->is_max_soc_offset = true;
		}
		BAT_INFO("OCV calib: cap=%d, rsoc=%d\n", ocv_cap, ocv_soc);
	}
finish:
	di->dsoc = pre_soc;
	di->nac = pre_cap;
	rk816_bat_init_capacity(di, di->nac);
	rk816_bat_set_initialized_state(di);
	BAT_INFO("dl=%d rl=%d cap=%d m=%d v=%d ov=%d c=%d pl=%d ch=%d Ver=%s\n",
		 di->dsoc, di->rsoc, di->remain_cap, di->pwroff_min,
		 rk816_bat_get_avg_voltage(di), rk816_bat_get_ocv_voltage(di),
		 rk816_bat_get_avg_current(di), rk816_bat_get_dsoc(di),
		 rk816_bat_get_usb_state(di), DRIVER_VERSION
		 );
}

static bool is_rk816_bat_first_poweron(struct battery_info *di)
{
	u8 buf;

	buf = rk816_bat_read(RK816_GGSTS_REG);
	if (buf & BAT_CON) {
		buf &= ~BAT_CON;
		rk816_bat_write(RK816_GGSTS_REG, buf);
		return true;
	}

	return false;
}

static bool rk816_bat_ocv_sw_reset(struct battery_info *di)
{
	u8 buf;

	buf = rk816_bat_read(RK816_MISC_MARK_REG);
	if (((buf & FG_RESET_LATE) && di->pwroff_min >= 30) ||
	    (buf & FG_RESET_NOW)) {
		buf &= ~FG_RESET_LATE;
		buf &= ~FG_RESET_NOW;
		rk816_bat_write(RK816_MISC_MARK_REG, buf);
		BAT_INFO("manual reset fuel gauge\n");
		return true;
	} else {
		return false;
	}
}

void rk816_bat_init_rsoc(struct battery_info *di)
{
	di->pwroff_min = rk816_bat_get_pwroff_min(di);
	di->is_first_power_on = is_rk816_bat_first_poweron(di);
	di->is_sw_reset = rk816_bat_ocv_sw_reset(di);

	if (di->is_first_power_on || di->is_sw_reset)
		rk816_bat_first_pwron(di);
	else
		rk816_bat_not_first_pwron(di);
}

static int rk816_bat_calc_linek(struct battery_info *di)
{
	int linek, diff, delta;

	di->calc_dsoc = di->dsoc;
	di->calc_rsoc = di->rsoc;
	di->sm_old_cap = di->remain_cap;

	delta = abs(di->dsoc - di->rsoc);
	diff = delta * 3;
	di->sm_meet_soc = (di->dsoc >= di->rsoc) ?
			   (di->dsoc + diff) : (di->rsoc + diff);

	if (di->dsoc < di->rsoc)
		linek = 1000 * (delta + diff) / DIV(diff);
	else if (di->dsoc > di->rsoc)
		linek = 1000 * diff / DIV(delta + diff);
	else
		linek = 1000;

	di->sm_chrg_dsoc = di->dsoc * 1000;

	DBG("<%s>. meet=%d, diff=%d, link=%d, calc: dsoc=%d, rsoc=%d\n",
	    __func__, di->sm_meet_soc, diff, linek,
	    di->calc_dsoc, di->calc_rsoc);

	return linek;
}

static int rk816_bat_get_coffset(struct battery_info *di)
{
	int val = 0;

	val |= rk816_bat_read(RK816_CAL_OFFSET_REGL) << 0;
	val |= rk816_bat_read(RK816_CAL_OFFSET_REGH) << 8;

	return val;
}

static void rk816_bat_init_poffset(struct battery_info *di)
{
	int coffset, ioffset;

	coffset = rk816_bat_get_coffset(di);
	ioffset = rk816_bat_get_ioffset(di);
	di->poffset = coffset - ioffset;
}

static void rk816_bat_fg_init(struct battery_info *di)
{
	rk816_bat_enable_gauge(di);
	rk816_bat_set_vol_instant_mode(di);
	rk816_bat_init_voltage_kb(di);
	rk816_bat_init_poffset(di);
	rk816_bat_clr_initialized_state(di);
	di->dsoc = rk816_bat_get_dsoc(di);

	/* it's better to init fg in kernel,
	 * so avoid init in uboot as far as possible
	 */
	if (rk816_bat_get_usb_state(di) != NO_CHARGER) {
		if (rk816_bat_get_est_voltage(di) < CONFIG_SCREEN_ON_VOL_THRESD)
			rk816_bat_init_rsoc(di);
#ifdef CONFIG_UBOOT_CHARGE
		else
			rk816_bat_init_rsoc(di);
#endif
	}

	rk816_bat_init_chrg_config(di);
	di->voltage_avg = rk816_bat_get_avg_voltage(di);
	di->voltage_ocv = rk816_bat_get_ocv_voltage(di);
	di->current_avg = rk816_bat_get_avg_current(di);
	di->sm_linek = rk816_bat_calc_linek(di);
	di->finish_chrg_base = get_timer(0);
	di->pwr_vol = di->voltage_avg;
}

static bool is_rk816_bat_exist(struct  battery_info *di)
{
	return (rk816_bat_read(RK816_SUP_STS_REG) & BAT_EXS) ? true : false;
}

static void rk816_bat_set_current(int input_current)
{
	u8 usb_ctrl;

	usb_ctrl = rk816_bat_read(RK816_USB_CTRL_REG);
	usb_ctrl &= ~INPUT_CUR_MSK;
	usb_ctrl |= (input_current);
	rk816_bat_write(RK816_USB_CTRL_REG, usb_ctrl);
}

static void rk816_bat_charger_setting(struct battery_info *di, int charger)
{
	static u8 old_charger = UNDEF_CHARGER;

	/*charger changed*/
	if (old_charger != charger) {
		if (charger == NO_CHARGER)
			rk816_bat_set_current(ILIM_450MA);
		else if (charger == USB_CHARGER)
			rk816_bat_set_current(ILIM_450MA);
		else if (charger == DC_CHARGER || charger == AC_CHARGER)
			rk816_bat_set_current(di->chrg_cur_input);
		else
			BAT_INFO("charger setting error %d\n", charger);

		old_charger = charger;
	}
}

static int rk816_bat_get_dc_state(struct battery_info *di)
{
	int val;

	if (di->dc_type == DC_TYPE_OF_NONE) {
		return NO_CHARGER;
	} else if (di->dc_type == DC_TYPE_OF_ADC) {
		val = saradc_get_value();
		return (val >= DC_ADC_TRIGGER) ? DC_CHARGER : NO_CHARGER;
	} else {
		return (gpio_get_value(di->dc_det.gpio) == di->dc_det.flags) ?
			DC_CHARGER : NO_CHARGER;
	}
}

static int rk816_bat_get_charger_type(struct battery_info *di,
				      struct pmic *pmic)
{
	int charger_type = NO_CHARGER;

	/* check by ic hardware: this check make check work safer */
	if ((rk816_bat_read(RK816_VB_MON_REG) & PLUG_IN_STS) == 0)
		return NO_CHARGER;

	/* virtual or bat not exist */
	if (di->virtual_power)
		return DC_CHARGER;

	/* check DC first */
	charger_type = rk816_bat_get_dc_state(di);
	if (charger_type == DC_CHARGER)
		return charger_type;

	/* check USB second */
	return rk816_bat_get_usb_state(di);
}

static int rk816_bat_check(struct pmic *p, struct pmic *bat)
{
	struct battery *battery = bat->pbat->bat;

	battery->state_of_chrg = rk816_bat_get_charger_type(&rk816_fg.di, bat);

	return 0;
}

static void rk816_bat_save_dsoc(struct  battery_info *di, u8 save_soc)
{
	static int old_soc = -1;

	if (old_soc != save_soc) {
		old_soc = save_soc;
		rk816_bat_write(RK816_SOC_REG, save_soc);
	}
}

static void rk816_bat_save_cap(struct battery_info *di, int cap)
{
	u8 buf;
	static int old_cap;

	if (old_cap == cap)
		return;

	if (cap >= di->qmax)
		cap = di->qmax;

	old_cap = cap;
	buf = (cap >> 24) & 0xff;
	rk816_bat_write(RK816_REMAIN_CAP_REG3, buf);
	buf = (cap >> 16) & 0xff;
	rk816_bat_write(RK816_REMAIN_CAP_REG2, buf);
	buf = (cap >> 8) & 0xff;
	rk816_bat_write(RK816_REMAIN_CAP_REG1, buf);
	buf = (cap >> 0) & 0xff;
	rk816_bat_write(RK816_REMAIN_CAP_REG0, buf);
}

static u8 rk816_bat_get_chrg_status(struct battery_info *di)
{
	u8 status;

	status = rk816_bat_read(RK816_SUP_STS_REG) & BAT_STATUS_MSK;
	switch (status) {
	case CHARGE_OFF:
		DBG("CHARGE-OFF...\n");
		break;
	case DEAD_CHARGE:
		DBG("DEAD CHARGE...\n");
		break;
	case  TRICKLE_CHARGE:
		DBG("TRICKLE CHARGE...\n ");
		break;
	case  CC_OR_CV:
		DBG("CC or CV...\n");
		break;
	case  CHARGE_FINISH:
		DBG("CHARGE FINISH...\n");
		break;
	case  USB_OVER_VOL:
		DBG("USB OVER VOL...\n");
		break;
	case  BAT_TMP_ERR:
		DBG("BAT TMP ERROR...\n");
		break;
	case  TIMER_ERR:
		DBG("TIMER ERROR...\n");
		break;
	case  USB_EXIST:
		DBG("USB EXIST...\n");
		break;
	case  USB_EFF:
		DBG(" USB EFF...\n");
		break;
	default:
		return -EINVAL;
	}

	return status;
}

static void rk816_bat_finish_chrg(struct battery_info *di)
{
	u32 tgt_sec = 0;

	if (di->dsoc < 100) {
		tgt_sec = di->fcc * 3600 / 100 / FINISH_CALI_CURR;
		if (get_timer(di->finish_chrg_base) > SECONDS(tgt_sec)) {
			di->finish_chrg_base = get_timer(0);
			di->dsoc++;
		}
	}
	DBG("<%s>. sec=%d, finish_sec=%lu\n", __func__, SECONDS(tgt_sec),
	    get_timer(di->finish_chrg_base));
}

static void rk816_bat_debug_info(struct battery_info *di)
{
	u8 sup_sts, ggcon, ggsts, vb_mod, rtc, thermal, misc;
	u8 usb_ctrl, chrg_ctrl1, chrg_ctrl2, chrg_ctrl3;
	const char *name[] = {"NONE", "USB", "AC", "DC", "UNDEF"};

	if (!dbg_enable)
		return;
	ggcon = rk816_bat_read(RK816_GGCON_REG);
	ggsts = rk816_bat_read(RK816_GGSTS_REG);
	sup_sts = rk816_bat_read(RK816_SUP_STS_REG);
	usb_ctrl = rk816_bat_read(RK816_USB_CTRL_REG);
	thermal = rk816_bat_read(RK816_THERMAL_REG);
	vb_mod = rk816_bat_read(RK816_VB_MON_REG);
	misc = rk816_bat_read(RK816_MISC_MARK_REG);
	rtc = rk816_bat_read(RK816_SECONDS_REG);
	chrg_ctrl1 = rk816_bat_read(RK816_CHRG_CTRL_REG1);
	chrg_ctrl2 = rk816_bat_read(RK816_CHRG_CTRL_REG2);
	chrg_ctrl3 = rk816_bat_read(RK816_CHRG_CTRL_REG3);

	DBG("\n---------------------- DEBUG REGS ------------------------\n"
	    "GGCON=0x%2x, GGSTS=0x%2x, RTC=0x%2x, SUP_STS= 0x%2x\n"
	    "VB_MOD=0x%2x, USB_CTRL=0x%2x, THERMAL=0x%2x, MISC=0x%2x\n"
	    "CHRG_CTRL:REG1=0x%2x, REG2=0x%2x, REG3=0x%2x\n",
	    ggcon, ggsts, rtc, sup_sts, vb_mod, usb_ctrl,
	    thermal, misc, chrg_ctrl1, chrg_ctrl2, chrg_ctrl3
	    );
	DBG("----------------------------------------------------------\n"
	    "Dsoc=%d, Rsoc=%d, Vavg=%d, Iavg=%d, Cap=%d, Fcc=%d, d=%d\n"
	    "K=%d, old_cap=%d, charger=%s, Is=%d, Ip=%d, Vs=%d\n"
	    "min=%d, meet: soc=%d, calc: dsoc=%d, rsoc=%d, Vocv=%d\n"
	    "off: i=0x%x, c=0x%x, max=%d, ocv_c=%d, halt: st=%d, cnt=%d\n"
	    "pwr: dsoc=%d, rsoc=%d, vol=%d, exist=%d\n",
	    di->dsoc, rk816_bat_get_rsoc(di), rk816_bat_get_avg_voltage(di),
	    rk816_bat_get_avg_current(di), di->remain_cap, di->fcc,
	    di->rsoc - di->dsoc,
	    di->sm_linek, di->sm_old_cap, name[rk816_fg.di.state_of_chrg],
	    CHRG_CUR_SEL[chrg_ctrl1 & 0x0f],
	    CHRG_CUR_INPUT[usb_ctrl & 0x0f],
	    CHRG_VOL_SEL[(chrg_ctrl1 & 0x70) >> 4],  di->pwroff_min,
	    di->sm_meet_soc, di->calc_dsoc, di->calc_rsoc,
	    rk816_bat_get_ocv_voltage(di), rk816_bat_get_ioffset(di),
	    rk816_bat_get_coffset(di), di->is_max_soc_offset,
	    di->is_ocv_calib, di->is_halt, di->halt_cnt, di->pwr_dsoc,
	    di->pwr_rsoc, di->pwr_vol, is_rk816_bat_exist(di)
	    );
	rk816_bat_get_chrg_status(di);
	DBG("###########################################################\n");
}

static void rk816_bat_linek_algorithm(struct battery_info *di)
{
	int delta_cap, ydsoc, tmp;
	u8 chg_st = rk816_bat_get_chrg_status(di);

	/* slow down */
	if (di->dsoc == 99)
		di->sm_linek = CHRG_FULL_K;
	else if (di->dsoc >= CHRG_TERM_DSOC && di->current_avg > TERM_CALI_CURR)
		di->sm_linek = CHRG_TERM_K;

	delta_cap = di->remain_cap - di->sm_old_cap;
	ydsoc = di->sm_linek * delta_cap * 100 / DIV(di->fcc);
	if (ydsoc > 0) {
		tmp = (di->sm_chrg_dsoc + 1) / 1000;
		if (tmp != di->dsoc)
			di->sm_chrg_dsoc = di->dsoc * 1000;
		di->sm_chrg_dsoc += ydsoc;
		di->dsoc = (di->sm_chrg_dsoc + 1) / 1000;
		di->sm_old_cap = di->remain_cap;
		if (di->dsoc == di->rsoc && di->sm_linek != CHRG_FULL_K &&
		    di->sm_linek != CHRG_TERM_K)
			di->sm_linek = 1000;
	}

	if ((di->sm_linek == 1000 || di->dsoc >= 100) &&
	    (chg_st != CHARGE_FINISH)) {
		if (di->sm_linek == 1000)
			di->dsoc = di->rsoc;
		di->sm_chrg_dsoc = di->dsoc * 1000;
	}

	DBG("linek=%d, sm_dsoc=%d, delta_cap=%d, ydsoc=%d, old_cap=%d\n"
	    "calc: dsoc=%d, rsoc=%d, meet=%d\n",
	    di->sm_linek, di->sm_chrg_dsoc, delta_cap, ydsoc, di->sm_old_cap,
	    di->calc_dsoc, di->calc_rsoc, di->sm_meet_soc);
}

static int rk816_bat_get_iadc(struct battery_info *di)
{
	int val = 0;

	val |= rk816_bat_read(RK816_BAT_CUR_AVG_REGL) << 0;
	val |= rk816_bat_read(RK816_BAT_CUR_AVG_REGH) << 8;
	if (val > 2047)
		val -= 4096;

	return val;
}

static bool rk816_bat_adc_calib(struct battery_info *di)
{
	int i, ioffset, coffset, adc;

	if (abs(di->current_avg) < ADC_CALIB_THRESHOLD)
		return false;

	for (i = 0; i < 5; i++) {
		adc = rk816_bat_get_iadc(di);
		coffset = rk816_bat_get_coffset(di);
		rk816_bat_set_coffset(di, coffset + adc);
		mdelay(200);
		adc = rk816_bat_get_iadc(di);
		if (abs(adc) < ADC_CALIB_THRESHOLD) {
			coffset = rk816_bat_get_coffset(di);
			ioffset = rk816_bat_get_ioffset(di);
			di->poffset = coffset - ioffset;
			rk816_bat_write(RK816_POFFSET_REG, di->poffset);
			BAT_INFO("new offset:c=0x%x, i=0x%x, p=0x%x\n",
				 coffset, ioffset, di->poffset);
			return true;
		} else {
			BAT_INFO("coffset calib again %d..\n", i);
			rk816_bat_set_coffset(di, coffset);
			mdelay(200);
		}
	}

	return false;
}
static void rk816_bat_smooth_charge(struct battery_info *di)
{
	u8 chg_st = rk816_bat_get_chrg_status(di);

	if (di->vol_mode_base && get_timer(di->vol_mode_base) > SECONDS(10)) {
		rk816_bat_set_vol_avg_mode(di);
		di->vol_mode_base = 0;
	}

	/* not charge mode and not keep in uboot charge: exit */
	if ((di->state_of_chrg == NO_CHARGER) ||
	    !rk816_bat_is_initialized(di)) {
		DBG("chrg=%d, initialized=%d\n", di->state_of_chrg,
		    rk816_bat_is_initialized(di));
		goto out;
	}

	/* update rsoc and remain cap */
	di->remain_cap = rk816_bat_get_coulomb_cap(di);
	di->rsoc = rk816_bat_get_rsoc(di);
	if (di->remain_cap > di->fcc) {
		di->sm_old_cap -= (di->remain_cap - di->fcc);
		rk816_bat_init_capacity(di, di->fcc);
	}

	/* finish charge step */
	if (chg_st == CHARGE_FINISH) {
		DBG("finish charge step...\n");
		if (di->adc_allow_update)
			di->adc_allow_update = !rk816_bat_adc_calib(di);
		rk816_bat_finish_chrg(di);
		rk816_bat_init_capacity(di, di->fcc);
	} else {
		DBG("smooth charge step...\n");
		di->adc_allow_update = true;
		di->finish_chrg_base = get_timer(0);
		rk816_bat_linek_algorithm(di);
	}

	/* dsoc limit */
	if (di->dsoc > 100)
		di->dsoc = 100;
	else if (di->dsoc < 0)
		di->dsoc = 0;

	rk816_bat_save_dsoc(di, di->dsoc);
	rk816_bat_save_cap(di, di->remain_cap);
out:
	rk816_bat_debug_info(di);
}

static int rk816_bat_update(struct pmic *p, struct pmic *bat)
{
	struct battery *battery = bat->pbat->bat;
	static ulong seconds;

	i2c_set_bus_num(bat->bus);
	i2c_init(RK816_I2C_SPEED, bat->hw.i2c.addr);
	/* set charge current */
	rk816_fg.di.state_of_chrg =
		rk816_bat_get_charger_type(&rk816_fg.di, bat);
	rk816_bat_charger_setting(&rk816_fg.di, rk816_fg.di.state_of_chrg);

	/* fg calc every 5 seconds */
	if (!seconds)
		seconds = get_timer(0);
	if (get_timer(seconds) >= SECONDS(5)) {
		seconds = get_timer(0);
		rk816_bat_smooth_charge(&rk816_fg.di);
	}

	/* bat exist, fg init success(dts pass) and uboot charge: report data */
	if (!rk816_fg.di.virtual_power && rk816_fg.di.voltage_k &&
	    rk816_bat_is_initialized(&rk816_fg.di)) {
		battery->voltage_uV = rk816_bat_get_est_voltage(&rk816_fg.di);
		battery->capacity = rk816_fg.di.dsoc;
	} else {
		battery->voltage_uV = VIRTUAL_POWER_VOL;
		battery->capacity = VIRTUAL_POWER_SOC;
	}

	battery->state_of_chrg = rk816_fg.di.state_of_chrg;
	//battery->isexistbat = 1;

	return 0;
}

static struct power_fg fg_ops = {
	.fg_battery_check = rk816_bat_check,
	.fg_battery_update = rk816_bat_update,
};

static int rk816_bat_parse_dt(struct battery_info *di, void const *blob)
{
	int node, parent;
	int len;
	int err;
	const char *prop;

	parent = fdt_node_offset_by_compatible(blob, 0,
					       COMPAT_ROCKCHIP_RK816);
	if (parent < 0) {
		printf("can't find rockchip,rk816 node\n");
		return -ENODEV;
	}

	if (!fdt_device_is_available(blob, parent)) {
		printf("device is disabled\n");
		return -EINVAL;
	}

	node = fdt_subnode_offset_namelen(blob, parent, "battery", 7);
	if (node < 0) {
		printf("can't find battery node\n");
		return -EINVAL;
	}

	prop = fdt_getprop(blob, node, "ocv_table", &len);
	if (!prop) {
		printf("can't find ocv_table prop\n");
		return -EINVAL;
	}

	di->ocv_table = calloc(len, 1);
	if (!di->ocv_table) {
		printf("calloc ocv_table fail\n");
		return -ENOMEM;
	}

	di->ocv_size = len / 4;
	err = fdtdec_get_int_array(blob, node, "ocv_table",
				   di->ocv_table, di->ocv_size);
	if (err < 0) {
		printf("read ocv_table error\n");
		free(di->ocv_table);
		return -EINVAL;
	}

	di->design_cap = fdtdec_get_int(blob, node, "design_capacity", -1);
	if (di->design_cap < 0) {
		printf("read design_capacity error\n");
		return -EINVAL;
	}

	di->qmax = fdtdec_get_int(blob, node, "design_qmax", -1);
	if (di->qmax < 0) {
		printf("read design_qmax error\n");
		return -EINVAL;
	}

	di->dts_vol_sel = fdtdec_get_int(blob, node, "max_chrg_voltage", 4200);
	di->dts_cur_input = fdtdec_get_int(blob, node, "max_input_current", 2000);
	di->dts_cur_sel = fdtdec_get_int(blob, node, "max_chrg_current", 1200);
	di->max_soc_offset = fdtdec_get_int(blob, node, "max_soc_offset", 70);
	di->virtual_power = fdtdec_get_int(blob, node, "virtual_power", 0);
	if (!is_rk816_bat_exist(di))
		di->virtual_power = 1;
	di->bat_res = fdtdec_get_int(blob, node, "bat_res", 135);
	di->dc_det_adc = fdtdec_get_int(blob, node, "dc_det_adc", 0);
	if (di->dc_det_adc <= 0) {
		if (!fdtdec_decode_gpio(blob, node,
					"dc_det_gpio", &di->dc_det)) {
			di->dc_det.flags =
				(di->dc_det.flags & OF_GPIO_ACTIVE_LOW) ? 0 : 1;
			gpio_direction_input(di->dc_det.gpio);
			di->dc_type = DC_TYPE_OF_GPIO;
		} else {
			di->dc_type = DC_TYPE_OF_NONE;
		}
	} else {
		di->dc_type = DC_TYPE_OF_ADC;
		saradc_init();
	}

	DBG("-------------------------------:\n");
	DBG("max_input_current:%d\n", di->dts_cur_input);
	DBG("max_chrg_current:%d\n", di->dts_cur_sel);
	DBG("max_chrg_voltage:%d\n", di->dts_vol_sel);
	DBG("design_capacity :%d\n", di->design_cap);
	DBG("design_qmax:%d\n", di->qmax);
	DBG("max_soc_offset:%d\n", di->max_soc_offset);
	DBG("dc_det_adc:%d\n", di->dc_det_adc);

	return 0;
}

int fg_rk816_init(unsigned char bus, uchar addr)
{
	static const char name[] = "RK816_FG";
	struct battery_info *di = &rk816_fg.di;
	int ret;

	if (!rk816_fg.p)
		rk816_fg.p = pmic_alloc();
	rk816_fg.p->name = name;
	rk816_fg.p->bus = bus;
	rk816_fg.p->hw.i2c.addr = addr;
	rk816_fg.p->interface = PMIC_I2C;
	rk816_fg.p->fg = &fg_ops;
	rk816_fg.p->pbat = calloc(sizeof(struct power_battery), 1);
	i2c_set_bus_num(bus);
	i2c_init(RK816_I2C_SPEED, addr);

	rk816_fg.adc.index = 0;
	rk816_fg.adc.data = SARADC_BASE;
	rk816_fg.adc.stas = SARADC_BASE + 4;
	rk816_fg.adc.ctrl = SARADC_BASE + 8;

	if (!gd->fdt_blob)
		return -ENODEV;

	ret = rk816_bat_parse_dt(di, gd->fdt_blob);
	if (ret < 0) {
		printf("rk816_bat_parse_dt failed!\n");
		return ret;
	}

	rk816_bat_fg_init(di);

	return 0;
}
