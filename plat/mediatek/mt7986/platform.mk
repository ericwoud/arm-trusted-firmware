#
# Copyright (c) 2021, MediaTek Inc. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#

MTK_PLAT		:=	plat/mediatek
MTK_PLAT_SOC		:=	${MTK_PLAT}/${PLAT}

# Whether to enble extra feature
I2C_SUPPORT		:= 	0
ENABLE_JTAG		?=	0

include lib/libfdt/libfdt.mk

PLAT_INCLUDES		:=	-I${MTK_PLAT}/common/				\
				-I${MTK_PLAT}/include/		\
				-I${MTK_PLAT}/common/drivers/uart		\
				-I${MTK_PLAT}/common/drivers/gpt		\
				\
				-Iinclude/plat/arm/common			\
				-Iinclude/plat/arm/common/aarch64		\
				-I${MTK_PLAT_SOC}/drivers/spmc/			\
				-I${MTK_PLAT_SOC}/drivers/timer/		\
				-I${MTK_PLAT_SOC}/drivers/gpio/			\
				-I${MTK_PLAT_SOC}/drivers/spi/			\
				-I${MTK_PLAT_SOC}/drivers/pll/			\
				-I${MTK_PLAT_SOC}/drivers/dram			\
				-I${MTK_PLAT_SOC}/drivers/devapc/		\
				-I${MTK_PLAT_SOC}/include/

PLAT_BL_COMMON_SOURCES	:=	lib/xlat_tables/xlat_tables_common.c		\
				lib/xlat_tables/aarch64/xlat_tables.c

DTS_NAME		:=	mt7986
FDT_SOURCES		+=	fdts/${DTS_NAME}.dts

BL2_SOURCES		:=	common/desc_image_load.c			\
				common/image_decompress.c			\
				drivers/delay_timer/delay_timer.c		\
				drivers/gpio/gpio.c				\
				drivers/io/io_storage.c				\
				drivers/io/io_block.c				\
				drivers/io/io_fip.c				\
				lib/cpus/aarch64/cortex_a53.S			\
					\
				${MTK_PLAT}/common/drivers/uart/aarch64/hsuart.S\
				${MTK_PLAT}/common/mtk_plat_common.c		\
				${MTK_PLAT}/common/drivers/gpt/mt_gpt.c		\
				\
				${MTK_PLAT_SOC}/aarch64/plat_helpers.S		\
				${MTK_PLAT_SOC}/aarch64/platform_common.c	\
				${MTK_PLAT_SOC}/dtb.S				\
				${MTK_PLAT_SOC}/bl2_plat_setup.c		\
				${MTK_PLAT_SOC}/drivers/timer/timer.c		\
				${MTK_PLAT_SOC}/drivers/gpio/mt7986_gpio.c	\
				${MTK_PLAT_SOC}/drivers/spi/boot_spi.c	\
				${MTK_PLAT_SOC}/drivers/pll/pll.c

CPPFLAGS		+=	-DDTB_PATH=\"${BUILD_PLAT}/fdts/${DTS_NAME}.dtb\"

# Whether supports DDR4
ifeq ($(DRAM_USE_DDR4), 1)
BL2_CPPFLAGS		+=	-DDRAM_USE_DDR4
AVAIL_DRAM_SIZE		:=	512 1024 2048
else
override DRAM_USE_DDR4	:=	0
AVAIL_DRAM_SIZE		:=	256 512
endif # END OF DRAM_USE_DDR4

# DDR4 frequency
ifeq ($(DDR4_FREQ_3200), 1)
BL2_CPPFLAGS		+=	-DDDR4_FREQ_3200
endif
ifeq ($(DDR4_FREQ_2666), 1)
BL2_CPPFLAGS		+=	-DDDR4_FREQ_2666
endif # END OF DDR4_FREQ_xxxx

# Whether to limit the DRAM size
ifdef DRAM_SIZE_LIMIT
ifneq ($(filter $(DRAM_SIZE_LIMIT),$(AVAIL_DRAM_SIZE)),)
BL2_CPPFLAGS		+=	-DDRAM_SIZE_LIMIT=$(DRAM_SIZE_LIMIT)
else
$(error You must specify the correct DRAM_SIZE_LIMIT. Avaliable values: $(AVAIL_DRAM_SIZE))
endif # END OF DRAM_SIZE_LIMIT check
else
DRAM_SIZE_LIMIT		:= 	0
endif # END OF DRAM_SIZE_LIMIT

# Whether to display verbose DRAM log
ifeq ($(DRAM_DEBUG_LOG), 1)
BL2_CPPFLAGS		+=	-DDRAM_DEBUG_LOG
endif # END OF DRAM_DEBUG_LOG

HAVE_DRAM_OBJ_FILE	:=	$(shell test -f ${MTK_PLAT_SOC}/drivers/dram/release/dram.o && echo yes)
ifeq ($(HAVE_DRAM_OBJ_FILE),yes)
PREBUILT_LIBS		+=	${MTK_PLAT_SOC}/drivers/dram/release/dram.o
BL2_SOURCES		+=	${MTK_PLAT_SOC}/drivers/dram/emicfg.c
else
PLAT_INCLUDES		+=	-I${MTK_PLAT_SOC}/drivers/dram/inc/

BL2_SOURCES		+=							\
	${MTK_PLAT_SOC}/drivers/dram/emicfg.c					\
	${MTK_PLAT_SOC}/drivers/dram/memory.c					\
	${MTK_PLAT_SOC}/drivers/dram/common/ANA_init_config.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/SRAM_init_lib.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/DIG_NONSHUF_config.c		\
	${MTK_PLAT_SOC}/drivers/dram/common/DIG_SHUF_config.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/DRAM_config_collection.c		\
	${MTK_PLAT_SOC}/drivers/dram/common/dramc_dv_freq_related.c		\
	${MTK_PLAT_SOC}/drivers/dram/common/dramc_dv_main.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/dramc_dvfs.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/DRAMC_SUBSYS_config.c		\
	${MTK_PLAT_SOC}/drivers/dram/common/dvsim_dummy.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/Hal_io.c				\
	${MTK_PLAT_SOC}/drivers/dram/common/sv_c_data_traffic.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/TX_RX_auto_gen_library.c		\
	${MTK_PLAT_SOC}/drivers/dram/common/TX_path_auto_gen.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/RX_path_auto_gen.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/HW_FUNC_MANAGE.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/ESL_DPHY_FPGA_TX_settings.c		\
	${MTK_PLAT_SOC}/drivers/dram/common/ESL_DPHY_FPGA_RX_settings.c		\
	${MTK_PLAT_SOC}/drivers/dram/common/dramc_dv_api.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/emi.c				\
	${MTK_PLAT_SOC}/drivers/dram/common/dramc_top.c				\
	${MTK_PLAT_SOC}/drivers/dram/common/dramc_actiming.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/dramc_utility.c			\
	${MTK_PLAT_SOC}/drivers/dram/common/MD32_initial.c			\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/dramc_pi_main.c			\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/dramc_pi_basic_api.c		\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/dramc_pi_calibration_api.c	\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/dramc_utility_pcddr.c		\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/dramc_slt.c			\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/dramc_tracking.c			\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/IPM_actiming_setting_DDR3.c	\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/IPM_actiming_setting_DDR4.c	\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/DDR3_dram_init.c			\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/DDR4_dram_init.c			\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/dramc_debug.c			\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/dramc_lowpower.c			\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/dramc_pi_ddr_reserve.c		\
	${MTK_PLAT_SOC}/drivers/dram/pcddr_cal/mtk_drm.c
endif # END OF HAVE_DRAM_OBJ_FILE

ifeq ($(I2C_SUPPORT), 1)
include ${MTK_PLAT}/common/drivers/i2c/i2c.mk
endif

ifeq ($(ENABLE_JTAG), 1)
CPPFLAGS		+=	-DENABLE_JTAG
endif

# Include GICv3 driver files
include drivers/arm/gic/v3/gicv3.mk

BL31_SOURCES		+=	drivers/arm/cci/cci.c				\
				${GICV3_SOURCES}				\
				drivers/delay_timer/delay_timer.c		\
				drivers/delay_timer/generic_delay_timer.c	\
				lib/cpus/aarch64/aem_generic.S			\
				lib/cpus/aarch64/cortex_a53.S			\
				plat/common/plat_gicv3.c			\
				${MTK_PLAT}/common/drivers/uart/aarch64/hsuart.S\
				${MTK_PLAT}/common/mtk_plat_common.c		\
				${MTK_PLAT}/common/drivers/gpt/mt_gpt.c		\
				\
				${MTK_PLAT}/common/mtk_sip_svc.c		\
				${MTK_PLAT_SOC}/aarch64/plat_helpers.S		\
				${MTK_PLAT_SOC}/aarch64/platform_common.c	\
				${MTK_PLAT_SOC}/bl31_plat_setup.c		\
				${MTK_PLAT_SOC}/plat_mt_gic.c			\
				${MTK_PLAT_SOC}/plat_pm.c			\
				${MTK_PLAT_SOC}/plat_sip_calls.c		\
				${MTK_PLAT_SOC}/drivers/spmc/mtspmc.c		\
				${MTK_PLAT_SOC}/drivers/timer/timer.c		\
				${MTK_PLAT_SOC}/drivers/spmc/mtspmc.c		\
				${MTK_PLAT_SOC}/drivers/gpio/mt7986_gpio.c	\
				${MTK_PLAT_SOC}/drivers/pll/pll.c		\
				${MTK_PLAT_SOC}/drivers/dram/emi_mpu.c		\
				${MTK_PLAT_SOC}/drivers/devapc/devapc.c		\
				${MTK_PLAT_SOC}/plat_topology.c

BL2_BASE		:=	0x201000
CPPFLAGS		+=	-DBL2_BASE=$(BL2_BASE)

# Enable workarounds for selected Cortex-A53 erratas.
ERRATA_A53_826319	:=	1
ERRATA_A53_836870	:=	1
ERRATA_A53_855873	:=	1

# indicate the reset vector address can be programmed
PROGRAMMABLE_RESET_ADDRESS	:=	1

$(eval $(call add_define,MTK_SIP_SET_AUTHORIZED_SECURE_REG_ENABLE))

# Do not enable SVE
ENABLE_SVE_FOR_NS		:=	0
MULTI_CONSOLE_API		:=	1

RESET_TO_BL2			:=	1

#
# Bromimage related build macros
#
DOIMAGEPATH		:=	tools/mediatek/bromimage
DOIMAGETOOL		:=	${DOIMAGEPATH}/bromimage


#
# Boot device related build macros
#
ifndef BOOT_DEVICE
$(echo You must specify the boot device by provide BOOT_DEVICE= to \
	make parameter. Avaliable values: nor emmc sdmmc snand)
else # NO BOOT_DEVICE
ifeq ($(BOOT_DEVICE),ram)
BL2_SOURCES		+=	drivers/io/io_memmap.c				\
				${MTK_PLAT_SOC}/bl2_boot_ram.c
ifeq ($(RAM_BOOT_DEBUGGER_HOOK), 1)
CPPFLAGS		+=	-DRAM_BOOT_DEBUGGER_HOOK
endif
ifeq ($(RAM_BOOT_UART_DL), 1)
BL2_SOURCES		+=	${MTK_PLAT}/common/uart_dl.c
CPPFLAGS		+=	-DRAM_BOOT_UART_DL
endif
endif # END OF BOOT_DEVICE = ram
ifeq ($(BOOT_DEVICE),nor)
CPPFLAGS		+=	-DMTK_SPIM_NOR
BROM_HEADER_TYPE	?=	nor
BL2_SOURCES		+=	drivers/mtd/nor/spi_nor.c			\
				drivers/mtd/spi-mem/spi_mem.c			\
				${MTK_PLAT_SOC}/bl2_boot_spim_nor.c
PLAT_INCLUDES		+=	-Iinclude/lib/libfdt
endif # END OF BOOTDEVICE = nor
ifeq ($(BOOT_DEVICE),emmc)
BL2_SOURCES		+=	drivers/mmc/mmc.c				\
				drivers/partition/partition.c			\
				drivers/partition/gpt.c				\
				common/tf_crc32.c				\
				${MTK_PLAT}/common/drivers/mmc/mtk-sd.c		\
				${MTK_PLAT_SOC}/bl2_boot_mmc.c
BROM_HEADER_TYPE	?=	emmc
CPPFLAGS		+=	-DMSDC_INDEX=0
BL2_CPPFLAGS		+=	-march=armv8-a+crc
endif # END OF BOOTDEVICE = emmc
ifeq ($(BOOT_DEVICE),sdmmc)
BL2_SOURCES		+=	drivers/mmc/mmc.c				\
				drivers/partition/partition.c			\
				drivers/partition/gpt.c				\
				common/tf_crc32.c				\
				${MTK_PLAT}/common/drivers/mmc/mtk-sd.c		\
				${MTK_PLAT_SOC}/bl2_boot_mmc.c
BROM_HEADER_TYPE	?=	sdmmc
CPPFLAGS		+=	-DMSDC_INDEX=1
BL2_CPPFLAGS		+=	-march=armv8-a+crc
endif # END OF BOOTDEVICE = sdmmc
ifeq ($(BOOT_DEVICE),snand)
include ${MTK_PLAT}/common/drivers/snfi/mtk-snand.mk
BROM_HEADER_TYPE	?=	snand
NAND_TYPE		?=	hsm:2k+64
BL2_SOURCES		+=	${MTK_SNAND_SOURCES}				\
				${MTK_PLAT_SOC}/bl2_boot_snand.c
PLAT_INCLUDES		+=	-I${MTK_PLAT}/common/drivers/snfi
				-DPRIVATE_MTK_SNAND_HEADER
endif # END OF BOOTDEVICE = snand (snfi)
ifeq ($(BOOT_DEVICE),spim-nand)
BROM_HEADER_TYPE	?=	spim-nand
NAND_TYPE		?=	spim:2k+64
BL2_SOURCES		+=	drivers/mtd/nand/core.c				\
				drivers/mtd/nand/spi_nand.c			\
				drivers/mtd/spi-mem/spi_mem.c			\
				${MTK_PLAT_SOC}/bl2_boot_spim_nand.c
PLAT_INCLUDES		+=	-Iinclude/lib/libfdt
endif # END OF BOOTDEVICE = spim-nand
ifeq ($(BROM_HEADER_TYPE),)
# $(error BOOT_DEVICE has invalid value. Please re-check.)
endif
endif # END OF BOOT_DEVICE

#
# Trusted board related build macros
#
ifneq (${TRUSTED_BOARD_BOOT},0)
include drivers/auth/mbedtls/mbedtls_crypto.mk
include drivers/auth/mbedtls/mbedtls_x509.mk
ifeq ($(MBEDTLS_DIR),)
$(error You must specify MBEDTLS_DIR when TRUSTED_BOARD_BOOT enabled)
endif
AUTH_SOURCES		:=	drivers/auth/auth_mod.c				\
				drivers/auth/crypto_mod.c			\
				drivers/auth/img_parser_mod.c			\
				drivers/auth/tbbr/tbbr_cot_bl2.c		\
				drivers/auth/tbbr/tbbr_cot_common.c
BL2_SOURCES		+=	${AUTH_SOURCES}					\
				${MTK_PLAT_SOC}/mtk_tbbr.c			\
				${MTK_PLAT_SOC}/mtk_rotpk.S
ROT_KEY			:=	$(BUILD_PLAT)/rot_key.pem
ROTPK_HASH		:=	$(BUILD_PLAT)/rotpk_sha256.bin

$(eval $(call add_define_val,ROTPK_HASH,'"$(ROTPK_HASH)"'))
$(BUILD_PLAT)/bl1/mtk_rotpk.o: $(ROTPK_HASH)
$(BUILD_PLAT)/bl2/mtk_rotpk.o: $(ROTPK_HASH)

certificates: $(ROT_KEY)
$(ROT_KEY): | $(BUILD_PLAT)
	@echo "  OPENSSL $@"
	$(Q)openssl genrsa 2048 > $@ 2>/dev/null

$(ROTPK_HASH): $(ROT_KEY)
	@echo "  OPENSSL $@"
	$(Q)openssl rsa -in $< -pubout -outform DER 2>/dev/null |\
	openssl dgst -sha256 -binary > $@ 2>/dev/null
endif

BL2_IMG_PAYLOAD := $(BUILD_PLAT)/bl2.bin

# Build dtb before embedding to BL2
${BUILD_PLAT}/bl2/dtb.o: ${BUILD_PLAT}/fdts/${DTS_NAME}.dtb

ifeq ($(BOOT_DEVICE),ram)
bl2: $(BL2_IMG_PAYLOAD)
else
bl2: $(BUILD_PLAT)/bl2.img
endif

ifneq ($(USE_MKIMAGE),1)
ifneq ($(BROM_SIGN_KEY),)
$(BUILD_PLAT)/bl2.img: $(BROM_SIGN_KEY)
endif

MTK_SIP_KERNEL_BOOT_ENABLE := 1
$(eval $(call add_define,MTK_SIP_KERNEL_BOOT_ENABLE))

$(BUILD_PLAT)/bl2.img: $(BL2_IMG_PAYLOAD) $(DOIMAGETOOL)
	-$(Q)rm -f $@.signkeyhash
	$(Q)$(DOIMAGETOOL) -c mt7986 -f $(BROM_HEADER_TYPE) -a $(BL2_BASE) -d -e	\
		$(if $(BROM_SIGN_KEY), -s sha256+rsa-pss -k $(BROM_SIGN_KEY))	\
		$(if $(BROM_SIGN_KEY), -p $@.signkeyhash)			\
		$(if $(NAND_TYPE), -n $(NAND_TYPE))				\
		$(BL2_IMG_PAYLOAD) $@
else
MKIMAGE ?= mkimage

ifneq ($(BROM_SIGN_KEY),)
$(warning BL2 signing is not supported using mkimage)
endif

$(BUILD_PLAT)/bl2.img: $(BL2_IMG_PAYLOAD)
	$(Q)$(MKIMAGE) -T mtk_image -a $(BL2_BASE) -e $(BL2_BASE)		\
		-n "arm64=1;media=$(BROM_HEADER_TYPE)$(if $(NAND_TYPE),;nandinfo=$(NAND_TYPE))"	\
		-d $(BL2_IMG_PAYLOAD) $@
endif

$(DOIMAGETOOL):
	$(Q)$(MAKE) --no-print-directory -C $(DOIMAGEPATH)

.PHONY: $(BUILD_PLAT)/bl2.img
