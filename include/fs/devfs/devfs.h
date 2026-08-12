/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * devfs.h
 *
 * Kernel-internal definitions for the DevFS character devices:
 * /dev/binder, /dev/hwbinder, /dev/vndbinder, /dev/ashmem (M3+).
 * This header is included only by the kext.
 */

#ifndef _FS_DEVFS_DEVFS_H_
#define _FS_DEVFS_DEVFS_H_

#include <fs/devfs/binder.h>

#pragma mark -
#pragma mark Kernel-Only Definitions

#ifndef __FSBUNDLE__

#include <kern/locks.h>
#include <libkern/OSMalloc.h>
#include <libkext.h>
#include <sys/kernel_types.h>
#include <sys/conf.h>      /* cdevsw, cdevsw_add, NO_CDEVICE */
#include <sys/queue.h>
#include <miscfs/devfs/devfs.h>  /* devfs_make_node, devfs_remove, DEVFS_CHAR, UID_ROOT, GID_WHEEL */

/*
 * Lock group name
 */
#define DEVFS_LCKGRP_NAME      BUNDLEID_S ".lckgrp"

/*
 * External references from other kext files (M1+)
 */
extern lck_grp_t *devfs_lck_grp;
extern lck_mtx_t *devfs_hash_mutex;
extern OSMallocTag devfs_osmalloc_tag;

/*
 * Per-process binder state (binder_proc) - forward declared for cdev handlers
 */
struct binder_proc;
struct binder_thread;
struct binder_node;
struct binder_ref;
struct binder_transaction;

/*
 * DevFS cdev sw table
 */
extern struct cdevsw devfs_cdevsw;

#endif /* __FSBUNDLE__ */

#endif /* _FS_DEVFS_DEVFS_H_ */