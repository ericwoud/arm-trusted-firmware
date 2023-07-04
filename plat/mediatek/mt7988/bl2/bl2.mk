#
# Copyright (c) 2023, MediaTek Inc. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#

include lib/libfdt/libfdt.mk
include lib/xz/xz.mk

# BL2 image options
BL2_IS_AARCH64		:=	1
BL2_SIG_TYPE		:=	sha256+rsa-pss
BL2_IMG_HDR_SOC		:=	mt7986

# Whether to enble extra feature
I2C_SUPPORT		:= 	0
EIP197_SUPPORT		:= 	0
ENABLE_JTAG		?=	0

FDT_SOURCES		+=	fdts/$(DTS_NAME).dts
BL2_CPPFLAGS		+=	-DDTB_PATH=\"$(BUILD_PLAT)/fdts/$(DTS_NAME).dtb\"

BL2_CPPFLAGS		+=	-I$(APSOC_COMMON)/drivers/spi			\
				-I$(MTK_PLAT_SOC)/drivers/gpio			\
				-I$(MTK_PLAT_SOC)/drivers/spi

BL2_SOURCES		+=	common/desc_image_load.c			\
				common/image_decompress.c			\
				drivers/io/io_storage.c				\
				drivers/io/io_block.c				\
				drivers/io/io_fip.c				\
				drivers/delay_timer/delay_timer.c		\
				drivers/delay_timer/generic_delay_timer.c	\
				drivers/gpio/gpio.c				\
				lib/cpus/aarch64/cortex_a73.S			\
				$(XZ_SOURCES)					\
				$(APSOC_COMMON)/drivers/efuse/mtk_efuse.c	\
				$(APSOC_COMMON)/drivers/spi/mtk_spi.c		\
				$(APSOC_COMMON)/drivers/uart/aarch64/hsuart.S	\
				$(APSOC_COMMON)/drivers/wdt/mtk_wdt.c		\
				$(MTK_PLAT_SOC)/aarch64/plat_helpers.S		\
				$(MTK_PLAT_SOC)/aarch64/platform_common.c	\
				$(MTK_PLAT_SOC)/bl2/dtb.S			\
				$(MTK_PLAT_SOC)/bl2/bl2_plat_init.c		\
				$(MTK_PLAT_SOC)/drivers/gpio/mt7988_gpio.c	\
				$(MTK_PLAT_SOC)/drivers/pll/pll.c		\
				$(MTK_PLAT_SOC)/drivers/spi/boot_spi.c		\
				$(MTK_PLAT_SOC)/drivers/timer/cpuxgpt.c

ifeq ($(I2C_SUPPORT), 1)
include $(APSOC_COMMON)/drivers/i2c/i2c.mk
override I2C_GPIO_SDA_PIN	:=	16
override I2C_GPIO_SCL_PIN	:=	15
BL2_SOURCES		+=	$(MTK_PLAT_SOC)/drivers/pmic/mt6682a.c
BL2_CPPFLAGS		+=	-I$(MTK_PLAT_SOC)/drivers/pmic
endif

ifeq ($(EIP197_SUPPORT), 1)
BL2_SOURCES		+=	$(MTK_PLAT_SOC)/drivers/eip197/eip197_init.c
BL2_CPPFLAGS		+=	-I$(MTK_PLAT_SOC)/drivers/eip197
BL2_CPPFLAGS		+=	-DEIP197_SUPPORT
endif

# Trusted board boot
include $(APSOC_COMMON)/bl2/tbbr.mk

ifeq ($(BL2_COMPRESS),1)
BL2_CPPFLAGS		+=	-DUSING_BL2PL
endif # END OF BL2_COMPRESS

ifeq ($(ENABLE_JTAG), 1)
BL2_CPPFLAGS		+=	-DENABLE_JTAG
endif

ifeq ($(BL2_CPU_FULL_SPEED), 1)
BL2_CPPFLAGS		+=	-DCPU_USE_FULL_SPEED
endif

BL2_BASE		:=	0x201000
BL2_CPPFLAGS		+=	-DBL2_BASE=$(BL2_BASE)

#
# Boot device related build macros
#
include $(APSOC_COMMON)/bl2/bl2_image.mk

ifndef BOOT_DEVICE
$(echo You must specify the boot device by provide BOOT_DEVICE= to \
	make parameter. Avaliable values: nor emmc sdmmc snand)
else # NO BOOT_DEVICE

ifeq ($(BOOT_DEVICE),ram)
$(eval $(call BL2_BOOT_RAM))
DTS_NAME		:=	mt7988
endif # END OF BOOT_DEVICE = ram

ifeq ($(BOOT_DEVICE),nor)
$(eval $(call BL2_BOOT_NOR))
BL2_SOURCES		+=	$(MTK_PLAT_SOC)/bl2/bl2_dev_spi_nor.c
DTS_NAME		:=	mt7988-spi2
endif # END OF BOOTDEVICE = nor

ifeq ($(BOOT_DEVICE),emmc)
$(eval $(call BL2_BOOT_EMMC))
BL2_SOURCES		+=	$(MTK_PLAT_SOC)/bl2/bl2_dev_mmc.c
BL2_CPPFLAGS		+=	-DMSDC_INDEX=0
DTS_NAME		:=	mt7988
endif # END OF BOOTDEVICE = emmc

ifeq ($(BOOT_DEVICE),sdmmc)
$(eval $(call BL2_BOOT_SD))
BL2_SOURCES		+=	$(MTK_PLAT_SOC)/bl2/bl2_dev_mmc.c
CPPFLAGS		+=	-DMSDC_INDEX=1
DTS_NAME		:=	mt7988
endif # END OF BOOTDEVICE = sdmmc

ifeq ($(BOOT_DEVICE),snand)
$(eval $(call BL2_BOOT_SNFI,0,0))
BL2_SOURCES		+=	$(MTK_PLAT_SOC)/bl2/bl2_dev_snfi.c
NAND_TYPE		?=	hsm20:2k+64
DTS_NAME		:=	mt7988
$(eval $(call BL2_BOOT_NAND_TYPE_CHECK,$(NAND_TYPE),hsm20:2k+64 hsm20:2k+128 hsm20:4k+256))
endif # END OF BOOTDEVICE = snand (snfi)

ifeq ($(BOOT_DEVICE),spim-nand)
$(eval $(call BL2_BOOT_SPI_NAND,0,0))
BL2_SOURCES		+=	$(MTK_PLAT_SOC)/bl2/bl2_dev_spi_nand.c
NAND_TYPE		?=	spim:2k+64
DTS_NAME		:=	mt7988-spi0
$(eval $(call BL2_BOOT_NAND_TYPE_CHECK,$(NAND_TYPE),spim:2k+64 spim:2k+128 spim:4k+256))
endif # END OF BOOTDEVICE = spim-nand

ifeq ($(BROM_HEADER_TYPE),)
$(error BOOT_DEVICE has invalid value. Please re-check.)
endif

endif # END OF BOOT_DEVICE
