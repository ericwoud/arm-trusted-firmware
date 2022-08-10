/*
 * Copyright (c) 2020, MediaTek Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <assert.h>
#include <drivers/delay_timer.h>
#include <drivers/io/io_driver.h>
#include <lib/utils_def.h>
#include <bl2_boot_dev.h>
#include <mt7986_def.h>
#include <mt7986_gpio.h>
#include <common/debug.h>
#include <plat/common/platform.h>
#include <lib/mmio.h>
#include <string.h>

#include <mtk-snand.h>
#include <mtk-snand-atf.h>

#define FIP_BASE			0x380000
#define FIP_SIZE			0x200000

static struct mtk_snand *snf;
static struct mtk_snand_chip_info cinfo;
static uint32_t oobavail;

static uint8_t *page_cache;

static size_t snand_read_range(int lba, uintptr_t buf, size_t size)
{
	uint64_t addr = lba * cinfo.pagesize;
	size_t retlen;

	mtk_snand_read_range(snf, addr, addr + FIP_SIZE, (void *)buf, size,
			     &retlen, page_cache);

	return retlen;
}

static size_t snand_write_range(int lba, uintptr_t buf, size_t size)
{
	/* Do not use write in BL2 */
	return 0;
}

static io_block_dev_spec_t snand_dev_spec = {
	/* Buffer should not exceed BL33_BASE */
	.buffer = {
		.offset = 0x41000000,
		.length = 0xe00000,
	},

	.ops = {
		.read = snand_read_range,
		.write = snand_write_range,
	},
};

const io_block_spec_t mtk_boot_dev_fip_spec = {
	.offset	= FIP_BASE,
	.length = FIP_SIZE,
};

static const struct mtk_snand_platdata mt7986_snand_pdata = {
	.nfi_base = (void *)NFI_BASE,
	.ecc_base = (void *)NFI_ECC_BASE,
	.soc = SNAND_SOC_MT7986,
	.quad_spi = true
};

static void snand_gpio_clk_setup(void)
{
	/* Reset */
	mmio_setbits_32(0x10001080, 1 << 2);
	udelay(1000);
	mmio_setbits_32(0x10001084, 1 << 2);

	/* TOPCKGEN CFG0 nfi1x */
	mmio_write_32(CLK_CFG_0_CLR, CLK_NFI1X_SEL_MASK);
	mmio_write_32(CLK_CFG_0_SET, CLK_NFI1X_52MHz << CLK_NFI1X_SEL_S);

	/* TOPCKGEN CFG0 spinfi */
	mmio_write_32(CLK_CFG_0_CLR, CLK_SPINFI_BCLK_SEL_MASK);
	mmio_write_32(CLK_CFG_0_SET, CLK_SPINFI_52MHz << CLK_SPINFI_BCLK_SEL_S);

	mmio_write_32(CLK_CFG_UPDATE, NFI1X_CK_UPDATE | SPINFI_CK_UPDATE);

	/* GPIO mode */
	mmio_clrsetbits_32(GPIO_MODE2, 0x7 << GPIO_PIN23_S, 0x1 << GPIO_PIN23_S);
	mmio_clrsetbits_32(GPIO_MODE3,
		0x7 << GPIO_PIN24_S | 0x7 << GPIO_PIN25_S | 0x7 << GPIO_PIN26_S |
		0x7 << GPIO_PIN27_S | 0x7 << GPIO_PIN28_S,
		0x1 << GPIO_PIN24_S | 0x1 << GPIO_PIN25_S | 0x1 << GPIO_PIN26_S |
		0x1 << GPIO_PIN27_S | 0x1 << GPIO_PIN28_S);

	/* GPIO PUPD */
	mmio_clrsetbits_32(GPIO_RT_PUPD_CFG0, 0b111111 << SPI0_PUPD_S, 0b011001 << SPI0_PUPD_S);
	mmio_clrsetbits_32(GPIO_RT_R0_CFG0, 0b111111 << SPI0_R0_S, 0b100110 << SPI0_R0_S);
	mmio_clrsetbits_32(GPIO_RT_R1_CFG0, 0b111111 << SPI0_R1_S, 0b011001 << SPI0_R1_S);

	/* GPIO driving */
	mmio_clrsetbits_32(GPIO_RT_DRV_CFG1,
		0x7 << SPI0_WP_DRV_S   | 0x7 << SPI0_MOSI_DRV_S | 0x7 << SPI0_MISO_DRV_S |
		0x7 << SPI0_HOLD_DRV_S | 0x7 << SPI0_CS_DRV_S   | 0x7 << SPI0_CLK_DRV_S,
		0x2 << SPI0_WP_DRV_S   | 0x2 << SPI0_MOSI_DRV_S | 0x2 << SPI0_MISO_DRV_S |
		0x2 << SPI0_HOLD_DRV_S | 0x2 << SPI0_CS_DRV_S   | 0x3 << SPI0_CLK_DRV_S);
}

static int mt7986_snand_init(void)
{
	int ret;

	snand_gpio_clk_setup();

	ret = mtk_snand_init(NULL, &mt7986_snand_pdata, &snf);
	if (ret) {
		printf("failed\n");
		snf = NULL;
		return ret;
	}

	mtk_snand_get_chip_info(snf, &cinfo);
	oobavail = cinfo.num_sectors * (cinfo.fdm_size - 1);
	snand_dev_spec.block_size = cinfo.pagesize;

	page_cache = mtk_mem_pool_alloc(cinfo.pagesize + cinfo.sparesize);

	NOTICE("SPI-NAND: %s (%luMB)\n", cinfo.model, cinfo.chipsize >> 20);

	return ret;
}

void mtk_boot_dev_setup(const io_dev_connector_t **boot_dev_con,
			uintptr_t *boot_dev_handle)
{
	int result;

	result = mt7986_snand_init();
	assert(result == 0);

	result = register_io_dev_block(boot_dev_con);
	assert(result == 0);

	result = io_dev_open(*boot_dev_con, (uintptr_t)&snand_dev_spec,
			     boot_dev_handle);
	assert(result == 0);

	/* Ignore improbable errors in release builds */
	(void)result;
}
