/*
 * (C) Copyright 2008-2015 Rockchip Electronics
 * Peter, Software Engineering, <superpeter.cai@gmail.com>.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <asm/io.h>
#include <asm/arch/rkplat.h>

DECLARE_GLOBAL_DATA_PTR;

#define RKRESET_VERSION		"1.3"

extern void FW_NandDeInit(void);


void rk_module_deinit(void)
{
#ifdef CONFIG_RK_I2C

#if defined(CONFIG_RKCHIP_RK3288)
	// soft reset i2c0 - i2c5
	writel(0x3f<<10 | 0x3f<<(10+16), RKIO_CRU_PHYS + CRU_SOFTRSTS_CON(2));
	mdelay(1);
	writel(0x00<<10 | 0x3f<<(10+16), RKIO_CRU_PHYS + CRU_SOFTRSTS_CON(2));
#elif defined(CONFIG_RKCHIP_RK3036) || defined(CONFIG_RKCHIP_RK3126) || defined(CONFIG_RKCHIP_RK3128)
	// soft reset i2c0 - i2c3
	writel(0x7<<11 | 0x7<<(11+16), RKIO_CRU_PHYS + CRU_SOFTRSTS_CON(2));
	mdelay(1);
	writel(0x00<<11 | 0x7<<(11+16), RKIO_CRU_PHYS + CRU_SOFTRSTS_CON(2));
#else
	#error "PLS config platform for i2c reset!"
#endif

#endif /* CONFIG_RK_I2C */

	/* rk pl330 dmac deinit */
#ifdef CONFIG_RK_DMAC
#ifdef CONFIG_RK_DMAC_0
	rk_pl330_dmac_deinit(0);
#endif
#ifdef CONFIG_RK_DMAC_1
	rk_pl330_dmac_deinit(1);
#endif
#endif /* CONFIG_RK_DMAC*/
}


/*
 * Reset the cpu by setting up the watchdog timer and let him time out.
 */

void reset_cpu(ulong ignored)
{
	disable_interrupts();
	FW_NandDeInit();

#ifndef CONFIG_SYS_L2CACHE_OFF
	v7_outer_cache_disable();
#endif
#ifndef CONFIG_SYS_DCACHE_OFF
	flush_dcache_all();
#endif
#ifndef CONFIG_SYS_ICACHE_OFF
	invalidate_icache_all();
#endif

#ifndef CONFIG_SYS_DCACHE_OFF
	dcache_disable();
#endif

#ifndef CONFIG_SYS_ICACHE_OFF
	icache_disable();
#endif

#if defined(CONFIG_RKCHIP_RK3288)
	/* pll enter slow mode */
	writel(PLL_MODE_SLOW(APLL_ID) | PLL_MODE_SLOW(GPLL_ID) | PLL_MODE_SLOW(CPLL_ID) | PLL_MODE_SLOW(NPLL_ID), RKIO_GRF_PHYS + CRU_MODE_CON);

	/* soft reset */
	writel(0xeca8, RKIO_CRU_PHYS + CRU_GLB_SRST_SND);
#elif defined(CONFIG_RKCHIP_RK3036)
	/* pll enter slow mode */
	writel(PLL_MODE_SLOW(APLL_ID) | PLL_MODE_SLOW(GPLL_ID), RKIO_GRF_PHYS + CRU_MODE_CON);

	/* soft reset */
	writel(0xeca8, RKIO_CRU_PHYS + CRU_GLB_SRST_SND);
#elif defined(CONFIG_RKCHIP_RK3126) || defined(CONFIG_RKCHIP_RK3128)
	/* pll enter slow mode */
	writel(PLL_MODE_SLOW(APLL_ID) | PLL_MODE_SLOW(CPLL_ID) | PLL_MODE_SLOW(GPLL_ID), RKIO_GRF_PHYS + CRU_MODE_CON);

	/* soft reset */
	writel(0xeca8, RKIO_CRU_PHYS + CRU_GLB_SRST_SND);
#else
	#error "PLS config platform for reset.c!"
#endif /* CONFIG_RKPLATFORM */
}

