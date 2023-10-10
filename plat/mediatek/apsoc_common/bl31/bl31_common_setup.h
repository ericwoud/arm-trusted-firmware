/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2023, MediaTek Inc. All rights reserved.
 */

#ifndef BL31_PLAT_SETUP_H
#define BL31_PLAT_SETUP_H

#include <stddef.h>

size_t mtk_bl31_get_dram_size(void);

#define BL33_INITRD_OFFSET  0x04000000
#define BL33_DTB_OFFSET     0x0AD00000
#define BL33_END_OFFSET     0x0B000000

#endif /* BL31_PLAT_SETUP_H */
