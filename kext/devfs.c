/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * devfs.c
 *
 * The mSL/DevFS kernel extension: Linux-compatible character devices for
 * Android userspace running under mSL/NABI. Today that means binder -
 * /dev/binder, /dev/hwbinder, /dev/vndbinder and the binderfs control
 * node - which is the one device with no macOS equivalent and no way to
 * emulate in userspace, because binder's whole purpose is to mediate
 * between processes that do not trust each other.
 *
 * This file is the module boundary: it takes the lock, the memory tag and
 * the character-device major, hands them to the binder driver, and gives
 * them back in the reverse order at unload. Everything binder does is in
 * binder_*.c; nothing here knows what a transaction is.
 *
 * The name is a nod to Linux's devfs rather than a claim to be one. macOS
 * already has a devfs and SIP refuses a second one stacked over /dev, so
 * these nodes are published into XNU's own, which is what devfs_make_node
 * is for.
 */

#include <fs/devfs/binder_internal.h>

#include <kern/locks.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/version.h>
#include <libkext.h>
#include <mach/kmod.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <os/log.h>
#include <sys/conf.h>
#include <sys/kernel_types.h>
#include <sys/sysctl.h>

#pragma mark -
#pragma mark External References

struct cdevsw *binder_cdevsw_ptr(void);
int android_devices_init(int binder_major);
void android_devices_fini(void);

#pragma mark -
#pragma mark Module State

lck_grp_t   *devfs_lck_grp = NULL;
lck_mtx_t   *binder_lock = NULL;
OSMallocTag  devfs_osmalloc_tag = NULL;

/* The major cdevsw_add() gave us. Kept because cdevsw_remove() needs it,
 * and passing anything else - d_type, say - silently fails to remove the
 * entry and leaks the major across an unload/reload cycle. */
static int g_devfs_major = -1;

#pragma mark -
#pragma mark Diagnostic sysctls

/*
 * Read-only counters for telling apart the three ways a binder driver goes
 * wrong while something is running against it:
 *
 *   sysctl devfs
 *
 *     devfs.binder_procs        opens of a binder device that are still live.
 *                               Should track the number of processes using
 *                               binder, and fall back to zero when they exit.
 *     devfs.binder_threads      per-thread states. Climbs to the size of the
 *                               clients' thread pools and plateaus; unbounded
 *                               growth means BINDER_THREAD_EXIT is not arriving.
 *     devfs.binder_nodes        live objects, devfs.binder_refs live handles.
 *                               These are the reference-counting protocol's
 *                               health: a leak here is a node whose owner was
 *                               never told to release it.
 *     devfs.binder_transactions transactions in flight. Should hover near zero;
 *                               a rising floor means replies are not coming.
 *     devfs.binder_arena_bytes  arena registered by clients, and
 *     devfs.binder_kdata_bytes  kernel memory holding payloads not yet freed.
 *                               The second is the cost of the copy model in
 *                               binder_alloc.c and should stay well under the
 *                               first; if it approaches it, clients are not
 *                               sending BC_FREE_BUFFER.
 *     devfs.binder_failed       transactions answered with an error return.
 *
 * Sample them a minute apart under load: whichever grows without bound
 * names the subsystem at fault.
 *
 * The oids are built by hand rather than with the SYSCTL_QUAD macros: under
 * XNU_KERNEL_PRIVATE, which this kext compiles with, those expand to a
 * STARTUP registration referencing sysctl_register_oid_early(), a symbol
 * not exported to kexts - so the kext would fail to bind and never load.
 * (The same fix as the sysfs and procfs siblings.)
 */
int64_t binder_stat_procs = 0;
int64_t binder_stat_threads = 0;
int64_t binder_stat_nodes = 0;
int64_t binder_stat_refs = 0;
int64_t binder_stat_transactions = 0;
int64_t binder_stat_arena_bytes = 0;
int64_t binder_stat_kdata_bytes = 0;
int64_t binder_stat_failed_transactions = 0;

static struct sysctl_oid_list devfs_sysctl_children;

static struct sysctl_oid devfs_sysctl_node = {
    .oid_parent  = &sysctl__children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_NODE | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &devfs_sysctl_children,
    .oid_arg2    = 0,
    .oid_name    = "devfs",
    .oid_handler = NULL,
    .oid_fmt     = "N",
    .oid_descr   = "mSL Linux-compatible character devices",
    .oid_version = SYSCTL_OID_VERSION,
};

#define DEVFS_STAT_OID(sym, var, oidname, desc)                               \
	static struct sysctl_oid sym = {                                          \
	    .oid_parent  = &devfs_sysctl_children,                                \
	    .oid_number  = OID_AUTO,                                              \
	    .oid_kind    = CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2, \
	    .oid_arg1    = &(var),                                                \
	    .oid_arg2    = 0,                                                     \
	    .oid_name    = (oidname),                                             \
	    .oid_handler = sysctl_handle_quad,                                    \
	    .oid_fmt     = "Q",                                                   \
	    .oid_descr   = (desc),                                                \
	    .oid_version = SYSCTL_OID_VERSION,                                    \
	}

DEVFS_STAT_OID(devfs_sysctl_procs, binder_stat_procs, "binder_procs",
    "live binder device opens");
DEVFS_STAT_OID(devfs_sysctl_threads, binder_stat_threads, "binder_threads",
    "per-thread binder states");
DEVFS_STAT_OID(devfs_sysctl_nodes, binder_stat_nodes, "binder_nodes",
    "live binder objects");
DEVFS_STAT_OID(devfs_sysctl_refs, binder_stat_refs, "binder_refs",
    "live binder handles");
DEVFS_STAT_OID(devfs_sysctl_transactions, binder_stat_transactions,
    "binder_transactions", "transactions in flight");
DEVFS_STAT_OID(devfs_sysctl_arena_bytes, binder_stat_arena_bytes,
    "binder_arena_bytes", "bytes of client-registered transaction arena");
DEVFS_STAT_OID(devfs_sysctl_kdata_bytes, binder_stat_kdata_bytes,
    "binder_kdata_bytes", "kernel bytes holding undelivered or unfreed payloads");
DEVFS_STAT_OID(devfs_sysctl_failed, binder_stat_failed_transactions,
    "binder_failed", "transactions answered with BR_FAILED_REPLY or BR_DEAD_REPLY");

static void
devfs_sysctl_register(void)
{
	sysctl_register_oid(&devfs_sysctl_node);   /* parent first */
	sysctl_register_oid(&devfs_sysctl_procs);
	sysctl_register_oid(&devfs_sysctl_threads);
	sysctl_register_oid(&devfs_sysctl_nodes);
	sysctl_register_oid(&devfs_sysctl_refs);
	sysctl_register_oid(&devfs_sysctl_transactions);
	sysctl_register_oid(&devfs_sysctl_arena_bytes);
	sysctl_register_oid(&devfs_sysctl_kdata_bytes);
	sysctl_register_oid(&devfs_sysctl_failed);
}

static void
devfs_sysctl_unregister(void)
{
	sysctl_unregister_oid(&devfs_sysctl_failed);
	sysctl_unregister_oid(&devfs_sysctl_kdata_bytes);
	sysctl_unregister_oid(&devfs_sysctl_arena_bytes);
	sysctl_unregister_oid(&devfs_sysctl_transactions);
	sysctl_unregister_oid(&devfs_sysctl_refs);
	sysctl_unregister_oid(&devfs_sysctl_nodes);
	sysctl_unregister_oid(&devfs_sysctl_threads);
	sysctl_unregister_oid(&devfs_sysctl_procs);
	sysctl_unregister_oid(&devfs_sysctl_node);
}

#pragma mark -
#pragma mark Module Initialization

kern_return_t
devfs_start(kmod_info_t *ki, __unused void *d)
{
	uuid_string_t uuid;
	lck_grp_attr_t *lck_grp_attr;
	int ret;

	os_log(OS_LOG_DEFAULT, "%s \n", version);

	ret = libkext_vma_uuid(ki->address, uuid);
	kassert(ret == 0);
	os_log(OS_LOG_DEFAULT, "kext executable uuid %s \n", uuid);

	lck_grp_attr = lck_grp_attr_alloc_init();
	if (lck_grp_attr == NULL) {
		os_log(OS_LOG_DEFAULT, "devfs: lck_grp_attr_alloc_init failed \n");
		return KERN_FAILURE;
	}
	devfs_lck_grp = lck_grp_alloc_init(DEVFS_LCKGRP_NAME, lck_grp_attr);
	lck_grp_attr_free(lck_grp_attr);
	if (devfs_lck_grp == NULL) {
		os_log(OS_LOG_DEFAULT, "devfs: lck_grp_alloc_init failed \n");
		return KERN_FAILURE;
	}

	binder_lock = lck_mtx_alloc_init(devfs_lck_grp, LCK_ATTR_NULL);
	if (binder_lock == NULL) {
		os_log(OS_LOG_DEFAULT, "devfs: lck_mtx_alloc_init failed \n");
		goto fail_grp;
	}

	devfs_osmalloc_tag = OSMalloc_Tagalloc(BUNDLEID_S, OSMT_DEFAULT);
	if (devfs_osmalloc_tag == NULL) {
		os_log(OS_LOG_DEFAULT, "devfs: OSMalloc_Tagalloc failed \n");
		goto fail_lock;
	}

	g_devfs_major = cdevsw_add(-1, binder_cdevsw_ptr());
	if (g_devfs_major == -1) {
		os_log(OS_LOG_DEFAULT, "devfs: cdevsw_add failed \n");
		goto fail_tag;
	}

	ret = binder_devices_init(g_devfs_major);
	if (ret != 0) {
		os_log(OS_LOG_DEFAULT, "devfs: binder_devices_init failed (%d) \n", ret);
		goto fail_cdevsw;
	}

	ret = android_devices_init(g_devfs_major);
	if (ret != 0) {
		os_log(OS_LOG_DEFAULT, "devfs: android_devices_init failed (%d) \n", ret);
		goto fail_android;
	}

	devfs_sysctl_register();

	os_log(OS_LOG_DEFAULT, "loaded %s version %s build %s (%s), binder major %d \n",
	    BUNDLEID_S, KEXTVERSION_S, KEXTBUILD_S, __TS__, g_devfs_major);
	return KERN_SUCCESS;

	/* Error Rollback Paths */
fail_cdevsw:
	cdevsw_remove(g_devfs_major, binder_cdevsw_ptr());
	g_devfs_major = -1;
fail_tag:
	OSMalloc_Tagfree(devfs_osmalloc_tag);
	devfs_osmalloc_tag = NULL;
fail_lock:
	lck_mtx_free(binder_lock, devfs_lck_grp);
	binder_lock = NULL;
fail_grp:
	lck_grp_free(devfs_lck_grp);
	devfs_lck_grp = NULL;
fail_android:
	return KERN_FAILURE;
}

kern_return_t
devfs_stop(__unused kmod_info_t *ki, __unused void *d)
{
	/*
	 * Reverse order: stop new work arriving (sysctls, then the device
	 * nodes, then the switch) before releasing what serves it.
	 *
	 * A kext with open device instances cannot be unloaded - the kernel
	 * refuses while any vnode references the major - so by the time this
	 * runs every binder_proc has already been through binder_proc_release.
	 */
	devfs_sysctl_unregister();
	android_devices_fini();
	binder_devices_fini();

	if (g_devfs_major != -1) {
		cdevsw_remove(g_devfs_major, binder_cdevsw_ptr());
		g_devfs_major = -1;
	}

	if (devfs_osmalloc_tag != NULL) {
		OSMalloc_Tagfree(devfs_osmalloc_tag);
		devfs_osmalloc_tag = NULL;
	}
	if (binder_lock != NULL) {
		lck_mtx_free(binder_lock, devfs_lck_grp);
		binder_lock = NULL;
	}
	if (devfs_lck_grp != NULL) {
		lck_grp_free(devfs_lck_grp);
		devfs_lck_grp = NULL;
	}

	libkext_massert();

	os_log(OS_LOG_DEFAULT, "unloaded %s version %s build %s (%s) \n",
	    BUNDLEID_S, KEXTVERSION_S, KEXTBUILD_S, __TS__);
	return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL (BUNDLEID_S, KEXTBUILD_S, devfs_start, devfs_stop)
  __attribute__ ((visibility ("default")))

__private_extern__ kmod_start_func_t *_realmain = devfs_start;
__private_extern__ kmod_stop_func_t  *_antimain = devfs_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
