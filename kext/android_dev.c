/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * android_dev.c
 *
 * Minimal Android character devices for mSL/NABI:
 *   /dev/__properties__  - property service ioctl stub
 *   /dev/kmsg            - kernel message log (empty)
 *
 * Both accept opens and fail unknown operations with ENOTTY or return
 * empty data. This is enough for Android init to get past its early
 * probes; a real implementation would hook into the property service
 * and kernel log buffer.
 */

#include <fs/devfs/binder_internal.h>

#include <libkern/libkern.h>
#include <libkext.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#pragma mark -
#pragma mark Property Device

/*
 * /dev/__properties__
 *
 * Android's init and property_service open this and talk to it via
 * ioctl. The real driver lives in the Android kernel; here we accept
 * the open so init does not crash on ENOENT, and return ENOTTY for
 * any ioctl we do not recognise.
 */

static int
prop_open(dev_t dev, int flags, int mode, struct proc *p)
{
	(void) dev;
	(void) flags;
	(void) mode;
	(void) p;
	return 0;
}

static int
prop_close(dev_t dev, int flags, int mode, struct proc *p)
{
	(void) dev;
	(void) flags;
	(void) mode;
	(void) p;
	return 0;
}

static int
prop_ioctl(dev_t dev, u_long cmd, caddr_t addr, int flags, struct proc *p)
{
	(void) dev;
	(void) addr;
	(void) flags;
	(void) p;

	/*
	 * Accept the known property ioctls silently. The property service
	 * is not running, so there is nothing to do, but init only checks
	 * that the call does not fail - it does not inspect the return value.
	 */
	switch (cmd) {
	case 0x30000: /* PROP_MSG_SET_PROPERTY */
	case 0x30001: /* PROP_MSG_GET_PROPERTY */
	case 0x30002: /* PROP_MSG_SET_FD */
		return 0;
	default:
		return ENOTTY;
	}
}

static struct cdevsw prop_cdevsw = {
	.d_open     = prop_open,
	.d_close    = prop_close,
	.d_read     = eno_rdwrt,
	.d_write    = eno_rdwrt,
	.d_ioctl    = prop_ioctl,
	.d_stop     = eno_stop,
	.d_reset    = eno_reset,
	.d_ttys     = NULL,
	.d_select   = eno_select,
	.d_mmap     = eno_mmap,
	.d_strategy = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type     = 0,
};

#pragma mark -
#pragma mark Kernel Message Device

/*
 * /dev/kmsg
 *
 * Android's init reads this for early kernel messages. The real device
 * is a ring buffer in the Linux kernel; here we return empty reads so
 * init gets EOF and moves on.
 */

static int
kmsg_open(dev_t dev, int flags, int mode, struct proc *p)
{
	(void) dev;
	(void) flags;
	(void) mode;
	(void) p;
	return 0;
}

static int
kmsg_close(dev_t dev, int flags, int mode, struct proc *p)
{
	(void) dev;
	(void) flags;
	(void) mode;
	(void) p;
	return 0;
}

static int
kmsg_read(dev_t dev, struct uio *uio, int flags)
{
	(void) dev;
	(void) flags;
	return 0; /* EOF: no kernel messages */
}

static int
kmsg_ioctl(dev_t dev, u_long cmd, caddr_t addr, int flags, struct proc *p)
{
	(void) dev;
	(void) cmd;
	(void) addr;
	(void) flags;
	(void) p;
	return ENOTTY;
}

static struct cdevsw kmsg_cdevsw = {
	.d_open     = kmsg_open,
	.d_close    = kmsg_close,
	.d_read     = kmsg_read,
	.d_write    = eno_rdwrt,
	.d_ioctl    = kmsg_ioctl,
	.d_stop     = eno_stop,
	.d_reset    = eno_reset,
	.d_ttys     = NULL,
	.d_select   = eno_select,
	.d_mmap     = eno_mmap,
	.d_strategy = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type     = 0,
};

#pragma mark -
#pragma mark Registration

static void *g_prop_handle = NULL;
static void *g_kmsg_handle = NULL;

int
android_devices_init(int binder_major)
{
	int major, ret;
	void *handle;

	major = cdevsw_add(-1, &prop_cdevsw);
	if (major == -1) {
		printf("devfs: android: could not get major for properties\n");
		return ENOMEM;
	}

	handle = devfs_make_node(makedev(major, 0), DEVFS_CHAR,
	    UID_ROOT, GID_WHEEL, 0666, "__properties__");
	if (handle == NULL) {
		printf("devfs: android: could not create /dev/__properties__\n");
		ret = ENOMEM;
		goto fail_prop;
	}
	g_prop_handle = handle;

 	major = cdevsw_add(-1, &kmsg_cdevsw);
 	if (major == -1) {
 		printf("devfs: android: could not get major for kmsg\n");
 		ret = ENOMEM;
 		goto fail_prop;
 	}
 
 	handle = devfs_make_node(makedev(major, 0), DEVFS_CHAR,
 	    UID_ROOT, GID_WHEEL, 0444, "kmsg");
 	if (handle == NULL) {
 		printf("devfs: android: could not create /dev/kmsg\n");
 		ret = ENOMEM;
 		cdevsw_remove(major, &kmsg_cdevsw);
 		goto fail_prop;
 	}
 	g_kmsg_handle = handle;
 
 	return 0;
 
 fail_prop:
 	if (g_prop_handle != NULL) {
 		devfs_remove(g_prop_handle);
 		g_prop_handle = NULL;
 	}
 	return ret;
 }

void
android_devices_fini(void)
{
	if (g_prop_handle != NULL) {
		devfs_remove(g_prop_handle);
		g_prop_handle = NULL;
	}
	if (g_kmsg_handle != NULL) {
		devfs_remove(g_kmsg_handle);
		g_kmsg_handle = NULL;
	}
}
