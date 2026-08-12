/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder.h
 *
 * Shared wire protocol for the Linux binder ABI on /dev/binder et al.
 * This header is included by both the kext and userspace tools/NABI.
 *
 * The binder wire structs and ioctl numbers here are the Linux binder ABI
 * on the wire (protocol version 8, scatter-gather + flat_binder_object
 * security ctx). The mSL-NABI include/linux/binder.h (M1) is the
 * cross-repo contract: both headers must be byte-identical on the wire
 * structs and ioctl values.
 */

#ifndef _FS_DEVFS_BINDER_H_
#define _FS_DEVFS_BINDER_H_

#include <stdint.h>

#pragma mark -
#pragma mark Binder Protocol Version

/*
 * Binder protocol version (the value BINDER_VERSION ioctl returns).
 * Protocol version 8 = scatter-gather + flat_binder_object security ctx.
 */
#define BINDER_CURRENT_PROTOCOL_VERSION 8

#pragma mark -
#pragma mark Binder Ioctl Numbers

/*
 * Binder ioctl numbers — Linux ABI verbatim.
 * _IOC(dir, type, nr, size) where type='b'=0x62.
 * Dir: _IOC_NONE=0, _IOC_WRITE=1, _IOC_READ=2
 */
#define BINDER_WRITE_READ        0xC0306201  /* _IOWR('b', 1, struct binder_write_read)  size 48 */
#define BINDER_SET_MAX_THREADS   0x40046205  /* _IOW ('b', 5, __u32)                   size 4  */
#define BINDER_SET_CONTEXT_MGR   0x40046207  /* _IOW ('b', 7, __s32)                   size 4  */
#define BINDER_SET_CONTEXT_MGR_EXT 0x4014620B  /* _IOW ('b', 11, struct flat_binder_object) size 20 */
#define BINDER_THREAD_EXIT       0x40046208  /* _IOW ('b', 8, __s32)                   size 4  */
#define BINDER_VERSION           0xC0186209  /* _IOWR('b', 9, struct binder_version)   size 24 */
#define BINDER_SET_IDLE_TIMEOUT  0x40086203  /* _IOW ('b', 3, int64_t)                 size 8  */
#define BINDER_SET_IDLE_PRIORITY 0x40046206  /* _IOW ('b', 6, __s32)                   size 4  */

#pragma mark -
#pragma mark Binder Wire Structs

/*
 * BINDER_VERSION ioctl argument structure.
 * Size per the ioctl encoding above (0x18 = 24 bytes).
 */
struct binder_version {
    int32_t protocol_version;
    int32_t reserved[5];  /* padding to 24 bytes per the ioctl number encoding */
};

#endif /* _FS_DEVFS_BINDER_H_ */