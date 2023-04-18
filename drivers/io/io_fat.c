/*
 * Copyright (c) 2014-2020, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Modified by Eric Woudstra
 *
 * This is a modified io_fip.c so that it reads from fat32 instead of a fip image.
 *
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <platform_def.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <drivers/io/io_driver.h>
#include <drivers/io/io_fat.h>
#include <drivers/io/io_storage.h>
#include <lib/utils.h>
#include <plat/common/platform.h>
#include <tools_share/firmware_image_package.h>
#include <tools_share/uuid.h>

#include <lib/fat32.h>

#ifndef MAX_FAT_DEVICES
#define MAX_FAT_DEVICES		1
#endif


/* Useful for printing UUIDs when debugging.*/
#define PRINT_UUID2(x)								\
	"%08x-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",	\
		x.time_low, x.time_mid, x.time_hi_and_version,			\
		x.clock_seq_hi_and_reserved, x.clock_seq_low,			\
		x.node[0], x.node[1], x.node[2], x.node[3],			\
		x.node[4], x.node[5]

extern struct BPB fat32_bs;
extern uint32_t *fat32_buffer;

typedef struct {
	unsigned int file_pos;
	DIR entry;
	bool opened;
} fat_file_state_t;

/*
 * Maintain dev_spec per FAT Device
 * TODO - Add backend handles and file state
 * per FAT device here once backends like io_memmap
 * can support multiple open files
 */
typedef struct {
	uintptr_t dev_spec;
	uint16_t some_unused_flag;
} fat_dev_state_t;

static const struct uuid_to_filename_table {
	char * name;
	uuid_t uuid;
} uuid_to_filename[] = {
	{
		.name = "bootcfg/bl31",
		.uuid = UUID_EL3_RUNTIME_FIRMWARE_BL31,
	},
	{
		.name = "bootcfg/linux",
		.uuid = UUID_NON_TRUSTED_FIRMWARE_BL33,
	},
	{
		.name = "bootcfg/atfdtb",
		.uuid = UUID_NT_FW_CONFIG,
	},
	{
		.name = "bootcfg/initrd",
		.uuid = UUID_SECURE_PAYLOAD_BL32_EXTRA2,
	},
	{
		.uuid = { {0} },
	}
};

/*
 * Only one file can be open across all FAT device
 * as backends like io_memmap don't support
 * multiple open files. The file state and
 * backend handle should be maintained per FAT device
 * if the same support is available in the backend
 */
static fat_file_state_t current_fat_file = {0};
static uintptr_t backend_dev_handle;
static uintptr_t backend_image_spec;

static fat_dev_state_t state_pool[MAX_FAT_DEVICES];
static io_dev_info_t dev_info_pool[MAX_FAT_DEVICES];

/* Track number of allocated fat devices */
static unsigned int fat_dev_count;

/* Firmware Image Package driver functions */
static int fat_dev_open(const uintptr_t dev_spec, io_dev_info_t **dev_info);
static int fat_file_open(io_dev_info_t *dev_info, const uintptr_t spec,
			  io_entity_t *entity);
static int fat_file_len(io_entity_t *entity, size_t *length);
static int fat_file_read(io_entity_t *entity, uintptr_t buffer, size_t length,
			  size_t *length_read);
static int fat_file_close(io_entity_t *entity);
static int fat_dev_init(io_dev_info_t *dev_info, const uintptr_t init_params);
static int fat_dev_close(io_dev_info_t *dev_info);


/* Return 0 for equal uuids. */
static inline int compare_uuids(const uuid_t *uuid1, const uuid_t *uuid2)
{
	return memcmp(uuid1, uuid2, sizeof(uuid_t));
}

/* Identify the device type as a virtual driver */
static io_type_t device_type_fat(void)
{
	return IO_TYPE_FIRMWARE_IMAGE_PACKAGE;
}

static const io_dev_connector_t fat_dev_connector = {
	.dev_open = fat_dev_open
};

static const io_dev_funcs_t fat_dev_funcs = {
	.type = device_type_fat,
	.open = fat_file_open,
	.seek = NULL,
	.size = fat_file_len,
	.read = fat_file_read,
	.write = NULL,
	.close = fat_file_close,
	.dev_init = fat_dev_init,
	.dev_close = fat_dev_close,
};

/* Locate a file state in the pool, specified by address */
static int find_first_fat_state(const uintptr_t dev_spec,
				  unsigned int *index_out)
{
	int result = -ENOENT;
	unsigned int index;

	for (index = 0; index < (unsigned int)MAX_FAT_DEVICES; ++index) {
		/* dev_spec is used as identifier since it's unique */
		if (state_pool[index].dev_spec == dev_spec) {
			result = 0;
			*index_out = index;
			break;
		}
	}
	return result;
}

/* Allocate a device info from the pool and return a pointer to it */
static int allocate_dev_info(io_dev_info_t **dev_info)
{
	int result = -ENOMEM;

	assert(dev_info != NULL);

	if (fat_dev_count < (unsigned int)MAX_FAT_DEVICES) {
		unsigned int index = 0;

		result = find_first_fat_state(0, &index);
		assert(result == 0);
		/* initialize dev_info */
		dev_info_pool[index].funcs = &fat_dev_funcs;
		dev_info_pool[index].info =
				(uintptr_t)&state_pool[index];
		*dev_info = &dev_info_pool[index];
		++fat_dev_count;
	}

	return result;
}

/* Release a device info to the pool */
static int free_dev_info(io_dev_info_t *dev_info)
{
	int result;
	unsigned int index = 0;
	fat_dev_state_t *state;

	assert(dev_info != NULL);

	state = (fat_dev_state_t *)dev_info->info;
	result = find_first_fat_state(state->dev_spec, &index);
	if (result ==  0) {
		/* free if device info is valid */
		zeromem(state, sizeof(fat_dev_state_t));
		--fat_dev_count;
	}

	return result;
}

/*
 * Multiple FAT devices can be opened depending on the value of
 * MAX_FAT_DEVICES. Given that there is only one backend, only a
 * single file can be open at a time by any FAT device.
 */
static int fat_dev_open(const uintptr_t dev_spec,
			 io_dev_info_t **dev_info)
{
	int result;
	io_dev_info_t *info;
	fat_dev_state_t *state;

	assert(dev_info != NULL);
#if MAX_FAT_DEVICES > 1
	assert(dev_spec != (uintptr_t)NULL);
#endif

	result = allocate_dev_info(&info);
	if (result != 0)
		return -ENOMEM;

	state = (fat_dev_state_t *)info->info;

	state->dev_spec = dev_spec;

	*dev_info = info;

	zeromem(&fat32_bs, sizeof(BPB));

	return 0;
}

/* Do some basic package checks. */
static int fat_dev_init(io_dev_info_t *dev_info, const uintptr_t init_params)
{
	int result;
	unsigned int image_id = (unsigned int)init_params;
	uintptr_t backend_handle;

	assert(dev_info != NULL);

	/* Obtain a reference to the image by querying the platform layer */
	result = plat_get_image_source(image_id, &backend_dev_handle,
				       &backend_image_spec);
	if (result != 0) {
		WARN("Failed to obtain reference to image id=%u (%i)\n",
			image_id, result);
		result = -ENOENT;
		goto fat_dev_init_exit;
	}

	/* Attempt to access the FAT image */
	result = io_open(backend_dev_handle, backend_image_spec,
			 &backend_handle);
	if (result != 0) {
		WARN("Failed to access image id=%u (%i)\n", image_id, result);
		result = -ENOENT;
		goto fat_dev_init_exit;
	}

	result = fat32_init(backend_handle);

	io_close(backend_handle);

 fat_dev_init_exit:
	return result;
}

/* Close a connection to the FAT device */
static int fat_dev_close(io_dev_info_t *dev_info)
{
	/* TODO: Consider tracking open files and cleaning them up here */

	/* Clear the backend. */
	backend_dev_handle = (uintptr_t)NULL;
	backend_image_spec = (uintptr_t)NULL;

	zeromem(&fat32_bs, sizeof(BPB));

	return free_dev_info(dev_info);
}


/* Open a file for access from package. */
static int fat_file_open(io_dev_info_t *dev_info, const uintptr_t spec,
			 io_entity_t *entity)
{
	int result;
	uintptr_t backend_handle;
	const io_uuid_spec_t *uuid_spec = (io_uuid_spec_t *)spec;
	static const uuid_t uuid_null = { {0} }; /* Double braces for clang */
	static const uuid_t uuid_bl33 = UUID_NON_TRUSTED_FIRMWARE_BL33;
	size_t bytes_read;
	bool found_file = false;
	int i = 0;
        DIR * entry;
        char filename[256];
        char * string;

	assert(uuid_spec != NULL);
	assert(entity != NULL);

	/* Can only have one file open at a time for the moment. We need to
	 * track state like file cursor position. We know the filename
	 * should never be zero for an active file.
	 * When the system supports dynamic memory allocation we can allow more
	 * than one open file at a time if needed.
	 */
	if (current_fat_file.opened == true) {
		WARN("fat_file_open: Only one open file at a time.\n");
		return -ENFILE;
	}

	/* Attempt to access the FAT image */
	result = io_open(backend_dev_handle, backend_image_spec,
			 &backend_handle);
	if (result != 0) {
		WARN("fat_file_open: Failed to open FAT32 partition (%i)\n", result);
		return -ENOENT;
	}

	entry = &current_fat_file.entry;

	// Try to load u-boot.bin
	if (compare_uuids(&uuid_bl33, &uuid_spec->uuid) == 0) {
		string = "u-boot.bin";
		result = fat32_open_file(backend_handle, string, entry);
		if (result == 0) {
			INFO("Opened %s\n", string);
			goto uboot_skip;
		}
	}

	// Load kernel, initrd and dtb from bootcfg/
	do {
		if (compare_uuids(&uuid_to_filename[i].uuid,
				  &uuid_spec->uuid) == 0) {
			found_file = true;
			break;
		}
	} while ((!found_file) && (compare_uuids(&uuid_to_filename[++i].uuid,
				  &uuid_null) != 0));
	if (!found_file) {
		WARN("fat_file_open: unknown uuid\n");
		goto fat_file_open_failed;
	}
	// Found the filename from uuid of the bootcfg/ file

	INFO("Reading filename from %s\n", uuid_to_filename[i].name);

	result = fat32_open_file(backend_handle, uuid_to_filename[i].name, entry);
	if (result != 0) {
		WARN("fat_file_open: failed opening %s\n", uuid_to_filename[i].name);
		goto fat_file_open_failed;
	}

	bytes_read = fat32_read_file(backend_handle, entry, filename, sizeof(filename)-1);
	if (bytes_read <= 0) {
		WARN("fat_file_open: failed 3\n");
		goto fat_file_open_failed;
	}
	filename[bytes_read] = '\0';

	// replace first \n with \0
	string = strchr(filename, '\n');
	if (string != NULL) *string = '\0';

	INFO("Opening (%s)\n", filename);

	// Remove leading /boot/ from filename
	if (memcmp(filename, "/boot/", 6) == 0) string = &filename[6];
	else                                    string =  filename;

	// Open the file that it is all about
	result = fat32_open_file(backend_handle, string, entry);
	if (result != 0) {
		WARN("fat_file_open: failed opening %s\n", filename);
		goto fat_file_open_failed;
	}

 uboot_skip:
	current_fat_file.file_pos = 0;
	current_fat_file.opened = true;
	entity->info = (uintptr_t)&current_fat_file;
	io_close(backend_handle);
	return 0;

 fat_file_open_failed:
	current_fat_file.file_pos = 0;
	current_fat_file.opened = false;
	io_close(backend_handle);
	return -ENOENT;
}


/* Return the size of a file in package */
static int fat_file_len(io_entity_t *entity, size_t *length)
{
        DIR * entry;

	assert(entity != NULL);
	assert(length != NULL);

        entry = &((fat_file_state_t *)entity->info)->entry;

	*length = fat32_file_size(entry);

	return 0;
}


/* Read data from a file in package */
static int fat_file_read(io_entity_t *entity, uintptr_t buffer, size_t length,
			  size_t *length_read)
{
	int result;
        DIR * entry;
	uintptr_t backend_handle;

	assert(entity != NULL);
	assert(length_read != NULL);
	assert(entity->info != (uintptr_t)NULL);

	/* Open the backend, attempt to access the blob image */
	result = io_open(backend_dev_handle, backend_image_spec,
			 &backend_handle);
	if (result != 0) {
		WARN("Failed to read FAT (%i)\n", result);
		return -EIO;
	}

        entry = &((fat_file_state_t *)entity->info)->entry;

	*length_read = fat32_read_file(backend_handle, entry, (char *) buffer, length);

	/* Close the backend. */
	io_close(backend_handle);

	return result;
}


/* Close a file in package */
static int fat_file_close(io_entity_t *entity)
{
	/* Clear our current file pointer.
	 * If we had malloc() we would free() here.
	 */
	if (current_fat_file.opened == true) {
		zeromem(&current_fat_file, sizeof(current_fat_file));
		current_fat_file.opened = false;
	}

	/* Clear the Entity info. */
	entity->info = 0;

	return 0;
}

/* Exported functions */

/* Register the Firmware Image Package driver with the IO abstraction */
int register_io_dev_fat(const io_dev_connector_t **dev_con)
{
	int result;
	assert(dev_con != NULL);

	/*
	 * Since dev_info isn't really used in io_register_device, always
	 * use the same device info at here instead.
	 */
	result = io_register_device(&dev_info_pool[0]);
	if (result == 0)
		*dev_con = &fat_dev_connector;

	return result;
}
