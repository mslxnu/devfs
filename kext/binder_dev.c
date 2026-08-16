/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder_dev.c
 *
 * The device nodes, and the cdevsw behind them.
 *
 * WHY THESE ARE CLONING DEVICES. Binder state is per-open: Linux hangs it
 * off the struct file, and tears it down when that file is released, which
 * is how a process that crashes has its objects declared dead. XNU calls
 * d_close only when the last reference to a dev_t goes away anywhere on
 * the system (spec_close, guarded by vcount()), so a single shared
 * /dev/binder would tell this driver nothing about individual processes
 * exiting - a hundred Android processes would share one close, arriving
 * whenever the last of them happened to finish. A cloning node hands every
 * lookup its own minor, and therefore its own close. It is what ptmx,
 * dtrace and auditpipe do for the same reason.
 *
 * The cost is devfs's rule that a *lookup* clones, not an open: stat()ing
 * /dev/binder consumes a minor that no close will ever return. The slot
 * table below therefore reclaims slots that were cloned but never opened
 * when it runs short, which is safe because every open performs its own
 * lookup and so never inherits a slot allocated for somebody's stat().
 *
 * CONTEXTS. /dev/binder, /dev/hwbinder and /dev/vndbinder are three
 * namespaces of the same driver: separate context managers, separate
 * nodes, separate handle numbering, no way to name an object in one from
 * another. Android uses the split to keep framework, HAL and vendor IPC
 * apart. The base minor of the cloned node says which context an open
 * belongs to; binderfs mints more of them at runtime.
 */

#include <fs/devfs/binder_internal.h>

#include <kern/thread.h>
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
#include <sys/types.h>

#pragma mark -
#pragma mark Minor Numbering

/*
 * Base minors name contexts; cloned minors name opens. Keeping them in
 * disjoint ranges means a dev_t alone says which kind it is, with no table
 * lookup and no ambiguity while the tables are being torn down.
 */
#define BINDER_CTL_MINOR    63          /* binderfs's binder-control       */
#define BINDER_CLONE_BASE   64          /* first per-open minor            */
#define BINDER_MAX_OPENS    448         /* 64..511, one per open device    */

struct binder_open_slot {
	bool cloned;                        /* a minor was handed out          */
	bool opened;                        /* and an open() followed          */
	pid_t lookup_pid;                   /* who asked, while unopened       */
	struct binder_context *context;
	struct binder_proc *proc;
	uint64_t sequence;                  /* for reclaiming stale clones     */
};

static struct binder_open_slot g_slots[BINDER_MAX_OPENS];
static uint64_t g_slot_sequence = 0;
static int g_binder_major = -1;

struct binder_context binder_contexts[BINDER_MAX_CONTEXTS];

struct binder_context *
binder_context_for_minor(int m)
{
	if (m < 0 || m >= BINDER_MAX_CONTEXTS) {
		return NULL;
	}
	return binder_contexts[m].in_use ? &binder_contexts[m] : NULL;
}

static struct binder_open_slot *
binder_slot_for_dev(dev_t dev)
{
	int m = minor(dev);

	if (m < BINDER_CLONE_BASE || m >= BINDER_CLONE_BASE + BINDER_MAX_OPENS) {
		return NULL;
	}
	return &g_slots[m - BINDER_CLONE_BASE];
}

/*
 * Hand out a minor for a new open. Called by devfs at lookup time, with
 * the *base* dev of the node being looked up - which is how the new open
 * learns its context.
 *
 * "At lookup time" is the trap, and it is worth being explicit about
 * because the first version of this walked straight into it. devfs clones
 * per *lookup*, not per open, so every stat() of /dev/binder minted a fresh
 * minor: five stats in a row reported five different rdevs, and anything
 * identifying the device that way - as mSL/NABI does when deciding whether
 * a descriptor is a binder descriptor - saw a different device each time it
 * looked. It also burned a slot per lookup, seven per stat(1) call and ten
 * per path resolution in the guest, so the table emptied in a few hundred.
 *
 * The callback runs in the looking-up thread's own context, so the fix is
 * to recognise the caller: a process that already holds a cloned but
 * unopened slot for this context gets that same slot back. Repeated stats
 * are then stable and cost one slot, and the slot is consumed by the open
 * that follows - which is the one the process was heading for.
 */
static int
binder_clone(dev_t dev, int action)
{
	struct binder_context *ctx;
	pid_t pid;
	int i, oldest = -1;
	uint64_t oldest_seq = ~0ull;

	if (action != DEVFS_CLONE_ALLOC) {
		return 0;
	}

	ctx = binder_context_for_minor(minor(dev));
	if (ctx == NULL) {
		return -1;
	}
	pid = proc_selfpid();

	BINDER_LOCK();

	/* This process's outstanding lookup, if it has one. */
	for (i = 0; i < BINDER_MAX_OPENS; i++) {
		if (g_slots[i].cloned && !g_slots[i].opened &&
		    g_slots[i].lookup_pid == pid && g_slots[i].context == ctx) {
			g_slots[i].sequence = ++g_slot_sequence;
			BINDER_UNLOCK();
			return BINDER_CLONE_BASE + i;
		}
	}

	for (i = 0; i < BINDER_MAX_OPENS; i++) {
		if (!g_slots[i].cloned) {
			break;
		}
		/* The longest-standing clone that never became an open. Safe to
		 * take back: every open performs its own lookup, so no open is
		 * waiting on a minor handed out earlier. */
		if (!g_slots[i].opened && g_slots[i].sequence < oldest_seq) {
			oldest_seq = g_slots[i].sequence;
			oldest = i;
		}
	}
	if (i == BINDER_MAX_OPENS) {
		if (oldest < 0) {
			BINDER_UNLOCK();
			printf("devfs: binder: out of open slots (%d in use)\n",
			    BINDER_MAX_OPENS);
			return -1;
		}
		i = oldest;
	}

	g_slots[i].cloned = true;
	g_slots[i].opened = false;
	g_slots[i].lookup_pid = pid;
	g_slots[i].context = ctx;
	g_slots[i].proc = NULL;
	g_slots[i].sequence = ++g_slot_sequence;
	BINDER_UNLOCK();

	return BINDER_CLONE_BASE + i;
}

#pragma mark -
#pragma mark cdevsw Handlers

static int
binder_dev_open(dev_t dev, int flags, int devtype, struct proc *p)
{
	struct binder_open_slot *slot = binder_slot_for_dev(dev);
	struct binder_proc *proc;

	if (slot == NULL) {
		/* A base minor: the context nodes themselves are not openable,
		 * only their clones, and binder-control is handled below. */
		if (minor(dev) == BINDER_CTL_MINOR) {
			return 0;
		}
		return ENXIO;
	}

	BINDER_LOCK();
	if (!slot->cloned || slot->context == NULL) {
		BINDER_UNLOCK();
		return ENXIO;
	}
	if (slot->opened) {
		/*
		 * Two opens raced on one cloned minor. Only reachable when two
		 * threads of the same process look up the same device at the
		 * same time, because a lookup now reuses that process's
		 * outstanding slot rather than minting a minor per lookup - the
		 * trade that makes rdev stable. Sequential opens are unaffected:
		 * the slot is released at close and the next lookup allocates a
		 * fresh one. Said out loud rather than returned silently,
		 * because a bare EBUSY here would be very hard to place.
		 */
		BINDER_UNLOCK();
		printf("devfs: binder: concurrent open of one cloned minor by "
		    "pid %d; the second is refused\n", proc_selfpid());
		return EBUSY;
	}
	proc = binder_proc_new(slot->context);
	if (proc == NULL) {
		BINDER_UNLOCK();
		return ENOMEM;
	}
	slot->proc = proc;
	slot->opened = true;
	slot->lookup_pid = 0;
	BINDER_UNLOCK();
	return 0;
}

static int
binder_dev_close(dev_t dev, int flags, int devtype, struct proc *p)
{
	struct binder_open_slot *slot = binder_slot_for_dev(dev);

	if (slot == NULL) {
		return 0;
	}

	BINDER_LOCK();
	if (slot->proc != NULL) {
		binder_proc_release(slot->proc);
		slot->proc = NULL;
	}
	slot->cloned = false;
	slot->opened = false;
	slot->lookup_pid = 0;
	slot->context = NULL;
	BINDER_UNLOCK();
	return 0;
}

/*
 * binderfs: mint a new context and publish a node for it. The caller names
 * it; the driver answers with the major and minor it was given, which is
 * what Linux's binderfs does after creating the file.
 */
static int
binder_ctl_add(struct binderfs_device *req)
{
	struct binder_context *ctx = NULL;
	void *handle;
	char name[BINDER_CONTEXT_NAME_MAX];
	int i;

	/* The name is the client's; make it a C string before it is used. */
	memcpy(name, req->name, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	if (name[0] == '\0' || strchr(name, '/') != NULL) {
		return EINVAL;
	}

	BINDER_LOCK();
	for (i = 0; i < BINDER_MAX_CONTEXTS; i++) {
		if (binder_contexts[i].in_use &&
		    strncmp(binder_contexts[i].name, name, sizeof(name)) == 0) {
			BINDER_UNLOCK();
			return EEXIST;
		}
	}
	for (i = 0; i < BINDER_MAX_CONTEXTS; i++) {
		if (!binder_contexts[i].in_use) {
			ctx = &binder_contexts[i];
			break;
		}
	}
	if (ctx == NULL) {
		BINDER_UNLOCK();
		return ENOSPC;
	}
	strlcpy(ctx->name, name, sizeof(ctx->name));
	ctx->minor = i;
	ctx->in_use = true;
	LIST_INIT(&ctx->procs);
	BINDER_UNLOCK();

	handle = devfs_make_node_clone(makedev(g_binder_major, i), DEVFS_CHAR,
	    UID_ROOT, GID_WHEEL, 0666, binder_clone, "binderfs/%s", name);
	if (handle == NULL) {
		BINDER_LOCK();
		ctx->in_use = false;
		BINDER_UNLOCK();
		return ENOMEM;
	}

	BINDER_LOCK();
	ctx->devfs_handle = handle;
	BINDER_UNLOCK();

	req->major = (uint32_t)g_binder_major;
	req->minor = (uint32_t)i;
	printf("devfs: binder: binderfs created /dev/binderfs/%s (%d,%d)\n",
	    name, g_binder_major, i);
	return 0;
}

/*
 * Does this command's argument have to have been copied in? XNU decides
 * that from the direction bits, and Linux numbers them the other way round
 * (see the file comment in binder.h), so a client that sends Linux's
 * literal number for a command that carries data gets a buffer of zeroes
 * rather than its own argument. That is worth failing loudly instead of
 * acting on: acting on zeroes here means registering a null context
 * manager or a zero-length arena.
 */
static bool
binder_cmd_reads_argument(u_long cmd)
{
	switch (BINDER_CMD_KEY(cmd)) {
	case BINDER_CMD_KEY(BINDER_WRITE_READ):
	case BINDER_CMD_KEY(BINDER_SET_MAX_THREADS):
	case BINDER_CMD_KEY(BINDER_SET_CONTEXT_MGR_EXT):
	case BINDER_CMD_KEY(BINDER_MSL_SET_ARENA):
	case BINDER_CMD_KEY(BINDER_CTL_ADD):
		return true;
	default:
		return false;
	}
}

static int
binder_dev_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag, struct proc *p)
{
	struct binder_open_slot *slot = binder_slot_for_dev(dev);
	struct binder_proc *proc;
	struct binder_thread *thread;
	int ret = 0;

	/* binderfs's control device serves exactly one command. */
	if (slot == NULL) {
		if (minor(dev) == BINDER_CTL_MINOR &&
		    BINDER_CMD_KEY(cmd) == BINDER_CMD_KEY(BINDER_CTL_ADD)) {
			if ((cmd & IOC_IN) == 0) {
				return EINVAL;
			}
			return binder_ctl_add((struct binderfs_device *)data);
		}
		return ENOTTY;
	}

	if (binder_cmd_reads_argument(cmd) && (cmd & IOC_IN) == 0) {
		printf("devfs: binder: command 0x%08lx sent with Linux direction "
		    "bits; use BINDER_CMD_HOST() (pid %d)\n", cmd, proc_selfpid());
		return EINVAL;
	}

	BINDER_LOCK();
	proc = slot->proc;
	if (proc == NULL || proc->is_dead) {
		BINDER_UNLOCK();
		return ENXIO;
	}
	thread = binder_thread_get(proc);
	if (thread == NULL) {
		BINDER_UNLOCK();
		return ENOMEM;
	}

	switch (BINDER_CMD_KEY(cmd)) {
	case BINDER_CMD_KEY(BINDER_VERSION): {
		struct binder_version *ver = (struct binder_version *)data;

		ver->protocol_version = BINDER_CURRENT_PROTOCOL_VERSION;
		break;
	}
	case BINDER_CMD_KEY(BINDER_MSL_ABI_VERSION):
		*(uint32_t *)data = BINDER_MSL_ABI_CURRENT;
		break;

	case BINDER_CMD_KEY(BINDER_MSL_SET_ARENA): {
		struct binder_msl_arena *arena = (struct binder_msl_arena *)data;

		ret = binder_arena_register(proc, arena->addr, arena->size);
		break;
	}
	case BINDER_CMD_KEY(BINDER_WRITE_READ):
		ret = binder_ioctl_write_read(proc, thread,
		    (struct binder_write_read *)data);
		break;

	case BINDER_CMD_KEY(BINDER_SET_MAX_THREADS): {
		uint32_t max = *(uint32_t *)data;

		if (max > 1024) {
			ret = EINVAL;
			break;
		}
		proc->max_threads = (int)max;
		break;
	}
	case BINDER_CMD_KEY(BINDER_SET_CONTEXT_MGR):
		ret = binder_context_mgr_set(proc, 0, 0, 0);
		break;

	case BINDER_CMD_KEY(BINDER_SET_CONTEXT_MGR_EXT): {
		struct flat_binder_object *fbo = (struct flat_binder_object *)data;

		ret = binder_context_mgr_set(proc, fbo->binder, fbo->cookie, fbo->flags);
		break;
	}
	case BINDER_CMD_KEY(BINDER_THREAD_EXIT):
		binder_thread_release(proc, thread);
		break;

	case BINDER_CMD_KEY(BINDER_GET_NODE_DEBUG_INFO): {
		struct binder_node_debug_info *info =
		    (struct binder_node_debug_info *)data;
		struct binder_node *node;
		binder_uintptr_t after = info->ptr;

		/* Walk the process's own nodes in address order, resuming from
		 * the pointer the caller was given last time. */
		bzero(info, sizeof(*info));
		LIST_FOREACH(node, &proc->nodes, proc_entry) {
			if (node->ptr <= after) {
				continue;
			}
			if (info->ptr == 0 || node->ptr < info->ptr) {
				info->ptr = node->ptr;
				info->cookie = node->cookie;
				info->has_strong_ref = node->has_strong_ref;
				info->has_weak_ref = node->has_weak_ref;
			}
		}
		break;
	}
	case BINDER_CMD_KEY(BINDER_GET_NODE_INFO_FOR_REF): {
		struct binder_node_info_for_ref *info =
		    (struct binder_node_info_for_ref *)data;
		struct binder_ref *ref = binder_ref_find_by_desc(proc, info->handle);

		if (ref == NULL || ref->node == NULL) {
			ret = EINVAL;
			break;
		}
		info->strong_count = (uint32_t)ref->node->internal_strong_refs +
		    (uint32_t)ref->node->local_strong_refs;
		info->weak_count = (uint32_t)ref->node->local_weak_refs;
		break;
	}
	case BINDER_CMD_KEY(BINDER_SET_IDLE_TIMEOUT):
	case BINDER_CMD_KEY(BINDER_SET_IDLE_PRIORITY):
		/* Accepted and ignored, as on Linux: both were part of the
		 * scheduling experiment that never shipped. */
		break;

	case BINDER_CMD_KEY(BINDER_FREEZE): {
		struct binder_freeze_info *info = (struct binder_freeze_info *)data;
		struct binder_thread *thread;
		bool found = false;

		LIST_FOREACH(thread, &proc->threads, entry) {
			if (thread->tid == info->pid) {
				thread->frozen = (info->enable != 0);
				thread->freeze_timeout_ms = info->timeout_ms;
				found = true;
				break;
			}
		}
		if (!found)
			ret = EINVAL;
		break;
	}
	case BINDER_CMD_KEY(BINDER_GET_FROZEN_INFO): {
		struct binder_frozen_status_info *info = (struct binder_frozen_status_info *)data;
		struct binder_thread *thread;
		bool found = false;

		info->sync_recv = 0;
		info->async_recv = 0;

		LIST_FOREACH(thread, &proc->threads, entry) {
			if (thread->tid == info->pid) {
				info->sync_recv = 0; /* TODO: track per-thread counts */
				info->async_recv = 0;
				found = true;
				break;
			}
		}
		if (!found)
			ret = EINVAL;
		break;
	}
	case BINDER_CMD_KEY(BINDER_ENABLE_ONEWAY_SPAM_DETECTION):
		proc->oneway_spam_detection = (*(uint32_t *)data != 0);
		break;
	case BINDER_CMD_KEY(BINDER_GET_EXTENDED_ERROR): {
		struct binder_extended_error *ee = (struct binder_extended_error *)data;
		*ee = proc->last_error;
		bzero(&proc->last_error, sizeof(proc->last_error));
		break;
	}

	default:
		ret = ENOTTY;
		break;
	}
	BINDER_UNLOCK();

	return ret;
}

/*
 * poll(). Android's looper watches the binder fd, so this has to agree
 * exactly with what the read loop would do - hence binder_has_work() is
 * the same predicate both use.
 */
static int
binder_dev_select(dev_t dev, int which, void *wql, struct proc *p)
{
	struct binder_open_slot *slot = binder_slot_for_dev(dev);
	struct binder_proc *proc;
	struct binder_thread *thread;
	int ret = 0;

	if (which == FWRITE) {
		return 1;   /* the write half never blocks */
	}
	if (which != FREAD || slot == NULL) {
		return 0;
	}

	BINDER_LOCK();
	proc = slot->proc;
	if (proc == NULL || proc->is_dead) {
		BINDER_UNLOCK();
		return 1;   /* report readable so the caller notices the error */
	}
	thread = binder_thread_lookup(proc, thread_tid(current_thread()));
	if (binder_has_work(proc, thread)) {
		ret = 1;
	} else {
		selrecord(p, &proc->selinfo, wql);
		proc->sel_recorded = true;
	}
	BINDER_UNLOCK();
	return ret;
}

#pragma mark -
#pragma mark Registration

static struct cdevsw binder_cdevsw = {
	.d_open     = binder_dev_open,
	.d_close    = binder_dev_close,
	.d_read     = eno_rdwrt,
	.d_write    = eno_rdwrt,
	.d_ioctl    = binder_dev_ioctl,
	.d_stop     = eno_stop,
	.d_reset    = eno_reset,
	.d_ttys     = NULL,
	.d_select   = binder_dev_select,
	.d_mmap     = eno_mmap,
	.d_strategy = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type     = 0,
};

/* The contexts Android expects to find already present. */
static const char *const binder_static_contexts[] = {
	"binder", "hwbinder", "vndbinder",
};

int
binder_devices_init(int major)
{
	size_t i;
	void *handle;

	g_binder_major = major;

	for (i = 0; i < sizeof(binder_static_contexts) / sizeof(binder_static_contexts[0]); i++) {
		struct binder_context *ctx = &binder_contexts[i];

		strlcpy(ctx->name, binder_static_contexts[i], sizeof(ctx->name));
		ctx->minor = (int)i;
		ctx->in_use = true;
		LIST_INIT(&ctx->procs);

		handle = devfs_make_node_clone(makedev(major, (int)i), DEVFS_CHAR,
		    UID_ROOT, GID_WHEEL, 0666, binder_clone, "%s", ctx->name);
		if (handle == NULL) {
			printf("devfs: binder: could not create /dev/%s\n", ctx->name);
			ctx->in_use = false;
			return ENOMEM;
		}
		ctx->devfs_handle = handle;
	}

	/*
	 * binderfs's control node. devfs takes a path, so this lands in a
	 * /dev/binderfs directory without a filesystem of its own - which is
	 * all binderfs is to a client: a place where binder-control lives and
	 * new devices appear beside it.
	 */
	handle = devfs_make_node(makedev(major, BINDER_CTL_MINOR), DEVFS_CHAR,
	    UID_ROOT, GID_WHEEL, 0600, "binderfs/binder-control");
	if (handle == NULL) {
		printf("devfs: binder: no /dev/binderfs/binder-control; "
		    "binderfs devices cannot be created\n");
	} else {
		binder_contexts[BINDER_MAX_CONTEXTS - 1].devfs_handle = handle;
	}

	return 0;
}

void
binder_devices_fini(void)
{
	int i;

	for (i = 0; i < BINDER_MAX_CONTEXTS; i++) {
		if (binder_contexts[i].devfs_handle != NULL) {
			devfs_remove(binder_contexts[i].devfs_handle);
			binder_contexts[i].devfs_handle = NULL;
		}
		binder_contexts[i].in_use = false;
	}
}

struct cdevsw *
binder_cdevsw_ptr(void)
{
	return &binder_cdevsw;
}
