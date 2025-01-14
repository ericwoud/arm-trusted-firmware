/*
 * Copyright (c) 2016-2022, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <stdint.h>

#include <arch.h>
#include <arch_helpers.h>
#include "bl2_private.h"
#include <common/bl_common.h>
#include <common/debug.h>
#include <common/tf_crc32.h>
#include <errno.h>
#include <common/desc_image_load.h>
#include <drivers/auth/auth_mod.h>
#include <plat/common/platform.h>

#include <platform_def.h>

__attribute__((weak)) int mtk_ar_update_bl_ar_ver(void)
{
	return 0;
}

extern void memcpy16(void *dest, const void *src, unsigned int length);
static int bl2_copy_image(const bl_load_info_node_t *bl2_node_info,
			    uintptr_t start, uintptr_t end, bool aligned)
{
	image_info_t *image_data = bl2_node_info->image_info;

	image_data->image_size = (uint32_t)(end - start);

	INFO("BL2: Copying id=%u from: 0x%lx to: 0x%lx size: 0x%lx\n",
		bl2_node_info->image_id, start,
		image_data->image_base,
		image_data->image_base + (uintptr_t)image_data->image_size);


	if (image_data->image_size > image_data->image_max_size) {
		ERROR("BL2: Image id=%u size out of bounds\n",
				bl2_node_info->image_id);
		return -EFBIG;
	}

	if (aligned)
		memcpy16((void *)image_data->image_base,
			 (void *)start, image_data->image_size);
	else
		memcpy((void *)image_data->image_base,
		       (void *)start, image_data->image_size);

	flush_dcache_range(image_data->image_base, image_data->image_size);

	return 0;
}

/*******************************************************************************
 * This function loads SCP_BL2/BL3x images and returns the ep_info for
 * the next executable image.
 ******************************************************************************/
struct entry_point_info *bl2_load_images(void)
{
	bl_params_t *bl2_to_next_bl_params;
	bl_load_info_t *bl2_load_info;
	const bl_load_info_node_t *bl2_node_info;
	int plat_setup_done = 0;
	int err;
	int index = 0;
	extern char _binary_bl31_bin_start[];
	extern char _binary_bl31_bin_end[];
	unsigned long long *atf_data = (unsigned long long *) BL31_LIMIT;
	uint32_t calc_crc=0, chck_crc=-1;

	/*
	 * Get information about the images to load.
	 */
	bl2_load_info = plat_get_bl_image_load_info();
	assert(bl2_load_info != NULL);
	assert(bl2_load_info->head != NULL);
	assert(bl2_load_info->h.type == PARAM_BL_LOAD_INFO);
	assert(bl2_load_info->h.version >= VERSION_2);
	bl2_node_info = bl2_load_info->head;

	while (bl2_node_info != NULL) {
		/*
		 * Perform platform setup before loading the image,
		 * if indicated in the image attributes AND if NOT
		 * already done before.
		 */
		if ((bl2_node_info->image_info->h.attr &
		    IMAGE_ATTRIB_PLAT_SETUP) != 0U) {
			if (plat_setup_done != 0) {
				WARN("BL2: Platform setup already done!!\n");
			} else {
				INFO("BL2: Doing platform setup\n");
				bl2_platform_setup();
				plat_setup_done = 1;
			}
		}

		if (index ==0) {
			calc_crc = tf_crc32(0U, (uint8_t *)atf_data, 31 * 8);
			chck_crc = atf_data[31];
			if (calc_crc == chck_crc) {
				atf_data[31] = 0; // invalidate crc
				flush_dcache_range((uintptr_t)atf_data, 32 * 8);
			}
		}

		err = bl2_plat_handle_pre_image_load(bl2_node_info->image_id);
		if (err != 0) {
			ERROR("BL2: Failure in pre image load handling (%i)\n", err);
			plat_error_handler(err);
		}

		if ((bl2_node_info->image_info->h.attr &
		    IMAGE_ATTRIB_SKIP_LOADING) == 0U) {

			// Load or copy pre-loaded
			if ((calc_crc == chck_crc) && (atf_data[2*index] != 0)) {
				err = bl2_copy_image(bl2_node_info,
					  (uintptr_t)(atf_data[2*index]),
					  (uintptr_t)(atf_data[2*index] +
						      atf_data[2*index+1]),
					  true);
			} else {
				INFO("BL2: Loading image id %u\n",
				     bl2_node_info->image_id);
				err = load_auth_image(bl2_node_info->image_id,
					bl2_node_info->image_info);
			}
			// Can boot kernel without initrd
			if ((err == -ENOENT) && (bl2_node_info->image_id
							== BL32_EXTRA2_IMAGE_ID)) {
				err = 0;
			}
			// Use build-in BL31 image if no image can be loaded
			if ((err != 0) && (bl2_node_info->image_id
							== BL31_IMAGE_ID)) {
				err = bl2_copy_image(bl2_node_info,
					  (uintptr_t)&_binary_bl31_bin_start,
					  (uintptr_t)&_binary_bl31_bin_end,
					  false);
			}

			index++;

			if (err != 0) {
				ERROR("BL2: Failed to load image id %u (%i)\n",
				      bl2_node_info->image_id, err);
				plat_error_handler(err);
			}
		} else {
			INFO("BL2: Skip loading image id %u\n", bl2_node_info->image_id);
		}

		/* Allow platform to handle image information. */
		err = bl2_plat_handle_post_image_load(bl2_node_info->image_id);
		if (err != 0) {
			ERROR("BL2: Failure in post image load handling (%i)\n", err);
			plat_error_handler(err);
		}

		/* Go to next image */
		bl2_node_info = bl2_node_info->next_load_info;
	}

	/*
	 * Get information to pass to the next image.
	 */
	bl2_to_next_bl_params = plat_get_next_bl_params();
	assert(bl2_to_next_bl_params != NULL);
	assert(bl2_to_next_bl_params->head != NULL);
	assert(bl2_to_next_bl_params->h.type == PARAM_BL_PARAMS);
	assert(bl2_to_next_bl_params->h.version >= VERSION_2);
	assert(bl2_to_next_bl_params->head->ep_info != NULL);

	/* Populate arg0 for the next BL image if not already provided */
	if (bl2_to_next_bl_params->head->ep_info->args.arg0 == (u_register_t)0)
		bl2_to_next_bl_params->head->ep_info->args.arg0 =
					(u_register_t)bl2_to_next_bl_params;

	/* Flush the parameters to be passed to next image */
	plat_flush_next_bl_params();

	/* Update boot loader anti-rollback version */
	err = mtk_ar_update_bl_ar_ver();
	if (err != 0) {
		ERROR("BL2: Failure in updating anti-rollback version (%i)\n", err);
		plat_error_handler(err);
	}

	return bl2_to_next_bl_params->head->ep_info;
}
