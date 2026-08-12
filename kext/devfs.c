/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * devfs.c
 *
 * The DevFS kernel extension: character devices /dev/binder et al.
 * for Android userspace under mSL/NABI.
 *
 * M0 scaffold: registers a cdevsw, creates /dev/binder, and answers
 * BINDER_VERSION (protocol 8). No binder protocol logic yet.
 */

#include <kern/locks.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/version.h>
#include <libkext.h>
#include <mach/kmod.h>
#include <mach/mach_types.h>
#include <os/log.h>
#include <sys/conf.h>
#include <sys/kernel_types.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/vnode.h>

#include <fs/devfs/devfs.h>

#pragma mark -
#pragma mark External References

/*
 * The seltrue function is used by NO_CDEVICE for the d_select slot.
 * Declared in <sys/systm.h> (not vendored); we forward-declare it here
 * to avoid pulling in systm.h's heavy dependencies.
 */
int seltrue(dev_t dev, int which, struct proc *p);

#pragma mark -
#pragma mark cdevsw Handlers

/*
 * Open: allocate per-open state. M0 is a stub - no state yet.
 * Return 0 on success.
 */
static int
devfs_open(dev_t dev, int flags, int devtype, struct proc *p)
{
#pragma unused(dev, flags, devtype, p)
    printf("devfs: open\n");
    return 0;
}

/*
 * Close: tear down per-open state, wake waiters, free transactions.
 * M0 stub: just log.
 */
static int
devfs_close(dev_t dev, int flags, int devtype, struct proc *p)
{
#pragma unused(dev, flags, devtype, p)
    printf("devfs: close\n");
    return 0;
}

/*
 * Ioctl: the Linux binder ioctl switch. M0 only answers BINDER_VERSION.
 * All other commands return ENOTTY.
 */
static int
devfs_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag, struct proc *p)
{
#pragma unused(dev, fflag, p)
    struct binder_version *ver = (struct binder_version *)data;

    switch (cmd) {
    case BINDER_VERSION:
        if (ver != NULL) {
            ver->protocol_version = BINDER_CURRENT_PROTOCOL_VERSION;
            ver->reserved[0] = 0;
            ver->reserved[1] = 0;
            ver->reserved[2] = 0;
            ver->reserved[3] = 0;
            ver->reserved[4] = 0;
        }
        printf("devfs: BINDER_VERSION -> %d\n", BINDER_CURRENT_PROTOCOL_VERSION);
        return 0;

    default:
        printf("devfs: ioctl 0x%lx not implemented (ENOTTY)\n", cmd);
        return ENOTTY;
    }
}

/*
 * The character device switch table.
 * Initialized from NO_CDEVICE (which uses seltrue for d_select),
 * then overridden with our handlers and d_type = 0 (no special type).
 */
struct cdevsw devfs_cdevsw = NO_CDEVICE;

/* Override the NO_CDEVICE defaults with our handlers. */
static void
devfs_cdevsw_init(void)
{
    devfs_cdevsw.d_open = devfs_open;
    devfs_cdevsw.d_close = devfs_close;
    devfs_cdevsw.d_ioctl = devfs_ioctl;
    devfs_cdevsw.d_type = 0;
}

#pragma mark -
#pragma mark Module Initialization

/*
 * Lock group name used by the kext.
 */
lck_grp_t *devfs_lck_grp = NULL;

/*
 * Memory allocation tag.
 */
OSMallocTag devfs_osmalloc_tag = NULL;

/*
 * Handle returned by devfs_make_node for /dev/binder, used in devfs_stop.
 */
static void *g_devfs_binder_handle = NULL;

/*
 * Kext start: register the cdev and devfs node.
 */
kern_return_t
devfs_start(kmod_info_t *ki, __unused void *d)
{
    uuid_string_t uuid;
    int error = 0;
    int major;
    void *devfs_handle = NULL;

    os_log(OS_LOG_DEFAULT, "%s \n", version);

    error = libkext_vma_uuid(ki->address, uuid);
    kassert(error == 0);

    os_log(OS_LOG_DEFAULT, "kext executable uuid %s \n", uuid);

    /* Allocate the lock group attribute. */
    lck_grp_attr_t *lck_grp_attr = lck_grp_attr_alloc_init();
    if (lck_grp_attr == NULL) {
        os_log(OS_LOG_DEFAULT, "devfs: lck_grp_attr_alloc_init failed\n");
        return KERN_FAILURE;
    }

    /* Allocate the lock group. */
    devfs_lck_grp = lck_grp_alloc_init(DEVFS_LCKGRP_NAME, lck_grp_attr);
    lck_grp_attr_free(lck_grp_attr);

    if (devfs_lck_grp == NULL) {
        os_log(OS_LOG_DEFAULT, "devfs: lck_grp_alloc_init failed\n");
        return KERN_FAILURE;
    }

    os_log(OS_LOG_DEFAULT, "lock group(%s) allocated \n", DEVFS_LCKGRP_NAME);

    /* Create the OSMalloc tag. */
    devfs_osmalloc_tag = OSMalloc_Tagalloc(BUNDLEID_S, OSMT_DEFAULT);
    if (devfs_osmalloc_tag == NULL) {
        os_log(OS_LOG_DEFAULT, "devfs: OSMalloc_Tagalloc failed\n");
        lck_grp_free(devfs_lck_grp);
        devfs_lck_grp = NULL;
        return KERN_FAILURE;
    }

    /* Register the character device switch. */
    devfs_cdevsw_init();
    major = cdevsw_add(-1, &devfs_cdevsw);
    if (major == -1) {
        os_log(OS_LOG_DEFAULT, "devfs: cdevsw_add failed\n");
        OSMalloc_Tagfree(devfs_osmalloc_tag);
        devfs_osmalloc_tag = NULL;
        lck_grp_free(devfs_lck_grp);
        devfs_lck_grp = NULL;
        return KERN_FAILURE;
    }

    os_log(OS_LOG_DEFAULT, "devfs: cdevsw_add got major %d\n", major);

    /* Create the devfs node /dev/binder (perms 0666, root:wheel). */
    g_devfs_binder_handle = devfs_make_node(makedev(major, 0), DEVFS_CHAR,
                                            UID_ROOT, GID_WHEEL, 0666,
                                            "binder", NULL);
    if (g_devfs_binder_handle == NULL) {
        os_log(OS_LOG_DEFAULT, "devfs: devfs_make_node failed\n");
        cdevsw_remove(major, &devfs_cdevsw);
        OSMalloc_Tagfree(devfs_osmalloc_tag);
        devfs_osmalloc_tag = NULL;
        lck_grp_free(devfs_lck_grp);
        devfs_lck_grp = NULL;
        return KERN_FAILURE;
    }

    os_log(OS_LOG_DEFAULT, "devfs: /dev/binder created\n");

    os_log(OS_LOG_DEFAULT, "loaded %s version %s build %s (%s) \n",
        BUNDLEID_S, KEXTVERSION_S, KEXTBUILD_S, __TS__);

    return KERN_SUCCESS;
}

/*
 * Kext stop: tear down the devfs node and cdevsw.
 */
kern_return_t
devfs_stop(__unused kmod_info_t *ki, __unused void *d)
{
    uuid_string_t uuid;
    kern_return_t ret = 0;

    ret = libkext_vma_uuid(ki->address, uuid);
    if (ret != 0) {
        os_log(OS_LOG_DEFAULT, "util_vma_uuid() failed  errno: %d \n", ret);
        return KERN_FAILURE;
    }

    /* Remove the devfs node. */
    devfs_remove(g_devfs_binder_handle);

    /* Remove the cdevsw entry. */
    cdevsw_remove(devfs_cdevsw.d_type, &devfs_cdevsw);

    /* Clean up. */
    if (devfs_osmalloc_tag != NULL) {
        OSMalloc_Tagfree(devfs_osmalloc_tag);
        devfs_osmalloc_tag = NULL;
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