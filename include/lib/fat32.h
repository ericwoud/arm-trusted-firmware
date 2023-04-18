
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

#ifndef FAT32_H
#define FAT32_H

#if defined(IMAGE_AT_EL1) || defined(IMAGE_AT_EL3)
#define BUILD4ATF
#define atfstatic static // Don't use the stack
#else
#define atfstatic        // Use the stack
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>

typedef struct BPB {
  uint8_t  BS_jmpBoot[3];
  uint8_t  BS_OEMName[8];
  uint16_t BPB_BytesPerSec;
  uint8_t  BPB_SecPerClus;
  uint16_t BPB_RsvdSecCnt;
  uint8_t  BPB_NumFATs;
  uint16_t BPB_RootEntCnt;
  uint16_t BPB_TotSec16;
  uint8_t  BPB_Media;
  uint16_t BPB_FATSz16;
  uint16_t BPB_SecPerTrk;
  uint16_t BPB_NumHeads;
  uint32_t BPB_HiddSec;
  uint32_t BPB_TotSec32;
  uint32_t BPB_FATSz32;
  uint16_t BPB_ExtFlags;
  uint16_t BPB_FSVer;
  uint32_t BPB_RootClus;
  uint16_t BPB_FSInfo;
  uint16_t BPB_BkBootSec;
  uint8_t  BPB_Reserved[12];
  uint8_t  BS_DrvNum;
  uint8_t  BS_Reserved1;
  uint8_t  BS_BootSig;
  uint32_t BS_VolID;
  uint8_t  BS_VolLab[11];
  uint8_t  BS_FilSysType[8];
  uint8_t  BS_CodeReserved[420];
  uint16_t BS_Sig;
}__attribute__((packed)) BPB;

typedef union DIR {
  struct {
    uint8_t  DIR_Name[11];
    uint8_t  DIR_Attr;
    uint8_t  DIR_NTRes;
    uint8_t  DIR_CrtTimeTenth;
    uint16_t DIR_CrtTime;
    uint16_t DIR_CrtDate;
    uint16_t DIR_LstAccDate;
    uint16_t DIR_FstClusHI;
    uint16_t DIR_WrtTime;
    uint16_t DIR_WrtDate;
    uint16_t DIR_FstClusLO;
    uint32_t DIR_FileSize;
  }__attribute__((packed)) S;
  struct {
    uint8_t  LDIR_Ord;
    uint8_t  LDIR_Name1[10];
    uint8_t  LDIR_Attr;
    uint8_t  LDIR_Type;
    uint8_t  LDIR_Chksum;
    uint8_t  LDIR_Name2[12];
    uint16_t LDIR_FstClusLO;
    uint8_t  LDIR_Name3[4];
  }__attribute__((packed)) L;
}__attribute__((packed)) DIR;

#define ATTR_READ_ONLY  0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LONG_NAME      (ATTR_READ_ONLY|ATTR_HIDDEN|ATTR_SYSTEM|ATTR_VOLUME_ID)
#define ATTR_LONG_NAME_MASK (ATTR_READ_ONLY|ATTR_HIDDEN|ATTR_SYSTEM|ATTR_VOLUME_ID|ATTR_DIRECTORY|ATTR_ARCHIVE)

#define FREE_DIR_ENTRY  0xE5
#define LAST_DIR_ENTRY  0x00
#define LAST_LONG_ENTRY 0x40

#define BAD_CLUSTER 0x0FFFFFF7

#define FAT32_MAX_LONG_NAME_LENGTH 255
#define FAT32_MAX_SECTOR_SIZE 512

int  fat32_open_file(const int handle, char *filename, DIR * entry);
int  fat32_read_file(const int handle, const DIR * entry, char *buf, size_t size);
int  fat32_file_size(const DIR * entry);
int  fat32_init(const int handle);
void fat32_free();
#ifdef BUILD4ATF
// fat32_list_entries() uses too much stack for ATF
#else // BUILD4LINUX
void fat32_list_entries(const int handle, uint32_t cluster, char *name);
#endif

#endif /* FAT32_H */
