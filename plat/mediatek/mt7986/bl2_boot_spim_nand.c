/*
 * Copyright (c) 2021, MediaTek Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <drivers/delay_timer.h>
#include <drivers/io/io_driver.h>
#include <lib/utils_def.h>
#include <bl2_boot_dev.h>
#include <mt7986_def.h>
#include <common/debug.h>
#include <plat/common/platform.h>
#include <lib/mmio.h>
#include <string.h>
#include <lib/utils.h>

#include <drivers/nand.h>
#include <drivers/spi_nand.h>
#include <mtk_spi.h>
#include <boot_spi.h>

#define FIP_BASE			0x380000
#define FIP_SIZE			0x200000

static size_t spim_nand_read_range(int lba, uintptr_t buf, size_t size)
{
	struct nand_device *nand_dev;
	size_t length_read;
	uint64_t off;
	int ret = 0;

	nand_dev = get_nand_device();
	if (nand_dev == NULL) {
		ERROR("spinand get device fail\n");
		return -EINVAL;
	}

	off = lba * nand_dev->page_size;
	ret = nand_read(off, buf, size, &length_read);
	if (ret < 0)
		ERROR("spinand read fail: %d, read length: %ld\n", ret, length_read);

	return length_read;
}

static size_t spim_nand_write_range(int lba, uintptr_t buf, size_t size)
{
	/* Do not use write in BL2 */
	return 0;
}

static io_block_dev_spec_t spim_nand_dev_spec = {
	/* Buffer should not exceed BL33_BASE */
	.buffer = {
		.offset = 0x41000000,
		.length = 0xe00000,
	},

	.ops = {
		.read = spim_nand_read_range,
		.write = spim_nand_write_range,
	},
};

const io_block_spec_t mtk_boot_dev_fip_spec = {
	.offset = FIP_BASE,
	.length = FIP_SIZE,
};

void mtk_boot_dev_setup(const io_dev_connector_t **boot_dev_con,
			uintptr_t *boot_dev_handle)
{
	struct nand_device *nand_dev;
	unsigned long long size;
	unsigned int erase_size;
	int result;

	/* config GPIO pinmux to spi mode */
	mtk_spi_gpio_init();

	/* select 208M clk */
	mtk_spi_source_clock_select(CLK_MPLL_D2);

	result = mtk_qspi_init(CLK_MPLL_D2);
	if (result) {
		ERROR("mtk spi init fail %d\n", result);
		assert(result == 0);
	}

	result = spi_nand_init(&size, &erase_size);
	if (result) {
		ERROR("spi nand init fail %d\n", result);
		assert(result == 0);
	}

	nand_dev = get_nand_device();
	assert(nand_dev == NULL);

	spim_nand_dev_spec.block_size = nand_dev->page_size;

	result = register_io_dev_block(boot_dev_con);
	assert(result == 0);

	result = io_dev_open(*boot_dev_con, (uintptr_t)&spim_nand_dev_spec,
			     boot_dev_handle);
	assert(result == 0);

	/* Ignore improbable errors in release builds */
	(void)result;
}
