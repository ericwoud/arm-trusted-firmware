
/*
 * Fat32 library extract a file from fat32 image. Can use long filenames.
 *
 * Copyright (C) 2023      Eric Woudstra
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License v2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <lib/fat32.h>

#ifdef BUILD4ATF
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <platform_def.h>
#include <drivers/io/io_driver.h>
#include <lib/utils.h>
#define toupper(c)  ((c) - 0x20 * (((c) >= 'a') && ((c) <= 'z')))
#define tolower(c)  ((c) + 0x20 * (((c) >= 'A') && ((c) <= 'Z')))
static int strcasecmp(const char *s1, const char *s2) {
  const unsigned char *us1 = (const unsigned char *)s1;
  const unsigned char *us2 = (const unsigned char *)s2;
  while (tolower(*us1) == tolower(*us2)) {
    if (*us1++ == '\0') return 0;
    us2++;
  }
  return tolower(*us1) - tolower(*us2);
}

#else // BUILD4LINUX
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IO_SEEK_SET 1
#define ERROR printf
#define NOTICE printf
#define WARN printf
#define INFO printf
#define VERBOSE printf
static int io_seek(uintptr_t handle, int mode, signed long long offset)
{
  if (mode != IO_SEEK_SET) return -1;
  int res = lseek(handle, offset, SEEK_SET);
  if (res < 0) return res;
  return 0;
}
static int io_read(uintptr_t handle, uintptr_t buffer, size_t length, size_t *length_read)
{
  *length_read = 0;
  int res = read(handle, (void *) buffer, length);
  if (res < 0) return res;
  *length_read = res;
  return 0;
}
static void zeromem(void * buffer, size_t length)
{
  memset(buffer,0,length);
}
size_t strlcpy(char * dst, const char * src, size_t dsize)
{
  strncpy(dst, src, dsize);
  dst[dsize] = '\0'; // make sure it is nul terminated
}
#endif

struct BPB fat32_bs;

#ifdef BUILD4ATF
uint32_t *fat32_buffer = (uint32_t *)FAT32BUFFER;
#else
uint32_t *fat32_buffer;
#endif

static int min3(const int s1,const int s2,const int s3) {
  int min = s1;
  if (min > s2) min = s2;
  if (min > s3) min = s3;
  return min;
}

static bool csum_direntry(const DIR * entry, const uint8_t csum) {
  const uint8_t *name = entry->S.DIR_Name;
  uint8_t sum = 0;
  for (int len=11; len!=0; len--) {
    sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *name++;
  }
  return (sum == csum);
}

static bool is_file_direntry(const DIR * entry) {
  return ((entry->S.DIR_Attr & ATTR_DIRECTORY) == 0);
}

static bool is_long_direntry(const DIR * entry) {
  return ((entry->L.LDIR_Attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME);
}

static bool is_free_direntry(const DIR * entry) {
  return (entry->S.DIR_Name[0] == FREE_DIR_ENTRY);
}

static bool is_last_direntry(const DIR * entry) {
  return (entry->S.DIR_Name[0] == LAST_DIR_ENTRY);
}

static bool is_volumeId_direntry(const DIR * entry) {
  return ((entry->S.DIR_Attr & ATTR_VOLUME_ID) != 0);
}

static uint32_t fstclus(const DIR * entry) {
  return ((entry->S.DIR_FstClusHI<<0x10) | entry->S.DIR_FstClusLO);
}

static void fill_long_name(const DIR * entry, char* dest, uint8_t* csum) {
  if (entry->L.LDIR_Ord & LAST_LONG_ENTRY) { // Shows up first
    zeromem(dest, FAT32_MAX_LONG_NAME_LENGTH);
    *csum = entry->L.LDIR_Chksum;
  }
  else if (*csum != entry->L.LDIR_Chksum) return;
  int ord = ((entry->L.LDIR_Ord & (LAST_LONG_ENTRY-1)) - 1) * 13;
  int i, k=0;
  for (i=0; i<10; i+=2, k++) dest[ord+k]=entry->L.LDIR_Name1[i];
  for (i=0; i<12; i+=2, k++) dest[ord+k]=entry->L.LDIR_Name2[i];
  for (i=0; i<4 ; i+=2, k++) dest[ord+k]=entry->L.LDIR_Name3[i];
}

static void fill_short_name(DIR * entry, char * dest) {
  int j = 0;
  bool appenddot = is_file_direntry(entry);
  if (entry->S.DIR_Name[8] == ' ') appenddot = false;
  for (int i = 0; i < 11; i++) {
    if (entry->S.DIR_Name[i] == '\0') break;
    if ((i == 8) && appenddot)     dest[j++] = '.';
    if (entry->S.DIR_Name[i] != ' ') dest[j++] = entry->S.DIR_Name[i];
  }
  dest[j] = '\0';
}

static uint64_t first_bytes_of_cluster(const uint32_t cluster) {
  uint64_t sector = fat32_bs.BPB_RsvdSecCnt +
                    fat32_bs.BPB_NumFATs*fat32_bs.BPB_FATSz32 +
                    fat32_bs.BPB_SecPerClus*(cluster-2);
  return (sector * fat32_bs.BPB_BytesPerSec);
}

static uint32_t read_fat(const int handle, const uint32_t cluster) {
  size_t len;
  if (fat32_buffer[cluster]) return fat32_buffer[cluster];
  uint64_t clussect=(sizeof(uint32_t)*cluster) & (~(fat32_bs.BPB_BytesPerSec-1));
  io_seek(handle, IO_SEEK_SET, fat32_bs.BPB_RsvdSecCnt*fat32_bs.BPB_BytesPerSec + clussect);
  io_read(handle,(uintptr_t)&fat32_buffer[clussect/sizeof(uint32_t)],fat32_bs.BPB_BytesPerSec, &len);
  return fat32_buffer[cluster];
}

static bool handle_entry(DIR * entry, char *lname, uint8_t *csum) {
  if (entry == NULL) goto handle_entry_abort;
  if (is_free_direntry(entry)) goto handle_entry_abort;
  if (is_long_direntry(entry)) {
    fill_long_name(entry, lname, csum);
    return false;
  } // After this: Short Dir Entry
  if (is_volumeId_direntry(entry)) goto handle_entry_abort;
  if ((lname[0] == '\0') || (!csum_direntry(entry, *csum))) fill_short_name(entry, lname);
  if (strcmp(lname,".") == 0) goto handle_entry_abort;
  if (strcmp(lname,"..") == 0) goto handle_entry_abort;
  return true; // handling entry needs to continue
 handle_entry_abort:
  lname[0] = '\0';
  return false;
}

static bool find_entry(const int handle, uint32_t cluster, char *name, DIR * entry) {
  uint8_t csum;
  atfstatic DIR entry_array[FAT32_MAX_SECTOR_SIZE/sizeof(DIR)];
  atfstatic char lname[FAT32_MAX_LONG_NAME_LENGTH];
  lname[0] = '\0';
  while (1) {
    io_seek(handle, IO_SEEK_SET, first_bytes_of_cluster(cluster));
    for (int i = 0; i < fat32_bs.BPB_SecPerClus; i++) {
      size_t len;
      io_read(handle, (uintptr_t)entry_array, fat32_bs.BPB_BytesPerSec, &len);
      for (int j = 0; j < fat32_bs.BPB_BytesPerSec/sizeof(DIR); j++) {
        if (is_last_direntry(&entry_array[j])) return false;
        if (!handle_entry(&entry_array[j], lname, &csum)) continue;
        if (strcasecmp(name, lname) != 0) continue;
        memcpy(entry, &entry_array[j], sizeof(DIR));
        return true;
      }
    }
    cluster=read_fat(handle, cluster);
    if (cluster >= BAD_CLUSTER) return false;
  }
}

int fat32_open_file(const int handle, char *filename, DIR * entry) {
  char *token, *name;
  uint32_t cluster = fat32_bs.BPB_RootClus;
  atfstatic char namebuffer[FAT32_MAX_LONG_NAME_LENGTH];
  strlcpy(namebuffer, filename, FAT32_MAX_LONG_NAME_LENGTH);
  name = namebuffer;
  while ((token = strtok_r(name, "/", &name))) {
    bool success = find_entry(handle, cluster, token, entry);
    if (!success) break;
    cluster = fstclus(entry);
    if (name != NULL) { if (name[0] == '\0') name = NULL; } // linux compatible
    if (name == NULL) {
      if (is_file_direntry(entry)) return 0;
      else break;
    }
    if (is_file_direntry(entry)) break; // continue if directory found
  }
  zeromem(entry, sizeof(DIR));
  return -1;
}

int fat32_file_size(const DIR * entry) {
  return entry->S.DIR_FileSize;
}

int fat32_read_file(const int handle, const DIR * entry, char *buf, size_t size) {
  uint32_t cluster = fstclus(entry);
  int filesize= fat32_file_size(entry);
  int count=0;
  size_t len;
  char *bf=buf;
  while(1)
  {
    int min=min3(size, filesize, fat32_bs.BPB_BytesPerSec*fat32_bs.BPB_SecPerClus);
    io_seek(handle, IO_SEEK_SET, first_bytes_of_cluster(cluster));
    io_read(handle,(uintptr_t)bf,min, &len);
    bf+=min; size-=min; filesize-=min; count+=min;
    if (size <= 0 || filesize <= 0) break;
    cluster=read_fat(handle, cluster);
    if (cluster >= BAD_CLUSTER) break;
  }
  VERBOSE("fat32_read_file: size read = %d\n", count);
  return count;
}

int fat32_init(const int handle) {
  size_t len;
  if (fat32_bs.BS_Sig != 0xAA55) {
    io_read(handle, (uintptr_t)&fat32_bs, sizeof(BPB), &len);
    VERBOSE("fat32_init: Name: %s\n", fat32_bs.BS_OEMName);
    VERBOSE("fat32_init: Bytes per Sector: %d\n", fat32_bs.BPB_BytesPerSec);
    VERBOSE("fat32_init: Sectors per Cluster: %d\n", fat32_bs.BPB_SecPerClus);
    VERBOSE("fat32_init: Number of FATs = %d\n", fat32_bs.BPB_NumFATs);
    VERBOSE("fat32_init: Number of Clusters: %ld\n",
             fat32_bs.BPB_FATSz32*fat32_bs.BPB_BytesPerSec/sizeof(uint32_t));
  }
  if (fat32_bs.BS_Sig != 0xAA55) {
    ERROR("FAT32: readBS: Boot Sector Signature Mismatch 0x%x != 0xAA55)\n", fat32_bs.BS_Sig);
    return -1;
  }
#ifdef BUILD4ATF
  zeromem(fat32_buffer, fat32_bs.BPB_FATSz32 * fat32_bs.BPB_BytesPerSec);
#else // BUILD4LINUX
  fat32_buffer = (uint32_t*)calloc(fat32_bs.BPB_FATSz32 , fat32_bs.BPB_BytesPerSec);
#endif
  return 0;
}

void fat32_free() {
#ifdef BUILD4ATF
#else // BUILD4LINUX
    if (fat32_buffer) free(fat32_buffer);
#endif
}

#ifdef BUILD4ATF
// fat32_list_entries() uses too much stack for ATF
#else // BUILD4LINUX
void fat32_list_entries(const int handle, uint32_t cluster, char *name) {
  DIR entry_array[FAT32_MAX_SECTOR_SIZE/sizeof(DIR)];
  char lname[FAT32_MAX_LONG_NAME_LENGTH] = {0};
  uint8_t csum = 0;
  while (1) {
    io_seek(handle, IO_SEEK_SET, first_bytes_of_cluster(cluster));
    for (int i = 0; i < fat32_bs.BPB_SecPerClus; i++) {
      size_t len;
      io_read(handle, (uintptr_t)entry_array, fat32_bs.BPB_BytesPerSec, &len);
      for (int j = 0; j < fat32_bs.BPB_BytesPerSec/sizeof(DIR); j++) {
        if (is_last_direntry(&entry_array[j])) return;
        if (!handle_entry(&entry_array[j], lname, &csum)) continue;
        if (is_file_direntry(&entry_array[j])) {
          VERBOSE("FAT32: find_entry: %s/%s\n", name, lname);
          continue;
        } // is Directory, go recursive
        char path[FAT32_MAX_LONG_NAME_LENGTH];
        snprintf(path, FAT32_MAX_LONG_NAME_LENGTH, "%s/%s", name, lname);
        fat32_list_entries(handle, fstclus(&entry_array[j]), path);
      }
    }
    cluster=read_fat(handle, cluster);
    if (cluster >= BAD_CLUSTER) return;
  }
}
#endif
