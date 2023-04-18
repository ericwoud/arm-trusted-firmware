/*
 * Copyright (c) 2014-2020, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef IO_FAT_H
#define IO_FAT_H

struct io_dev_connector;

int register_io_dev_fat(const struct io_dev_connector **dev_con);

#endif /* IO_FAT_H */
