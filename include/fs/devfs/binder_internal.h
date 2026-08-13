/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder_internal.h
 *
 * The driver's own state: what a binder process, thread, node, reference,
 * buffer and transaction are on this side of the ioctl. None of this is
 * ABI - <fs/devfs/binder.h> holds everything a client can see - and it is
 * modelled on Linux's drivers/android/binder.c closely enough that its
 * documentation reads across, with three deliberate departures:
 *
 * ONE LOCK. Linux has held per-proc inner/outer locks since 4.9, having
 * started where this starts, with a single global mutex. The reason to
 * start here anyway is that binder's state is a graph - a transaction
 * touches the sender's thread, the target's proc, a node owned by a third
 * process and a reference owned by a fourth - and the lock ordering that
 * makes fine-grained locking safe is the part that is hard to get right,
 * not the locking itself. Contention is visible through the devfs.binder
 * sysctls; the day it matters, the split is per-proc first. Every sleep
 * drops this lock (see binder_wait), so holding it across a wait is not a
 * hazard, and holding it across copyin/copyout is legal because it is a
 * mutex and a fault may block.
 *
 * NO KERNEL MAPPING. Linux allocates each process a buffer at mmap() time
 * and writes transaction payloads straight into it, which is what makes
 * binder single-copy. macOS has no mmap for character devices at all
 * (bsd/kern/kern_mman.c answers ENODEV for every VCHR vnode, and d_mmap
 * has no callers anywhere in the kernel), so the client registers memory
 * it allocated itself - BINDER_MSL_SET_ARENA - and the driver holds the
 * payload in kernel memory between send and receive, copying it out into
 * that region in the receiving thread's own context, which is the one
 * place a copyout to that address is valid. The allocator below still
 * hands out offsets within the region exactly as Linux's does, so
 * BC_FREE_BUFFER and the buffer pointers inside BR_TRANSACTION keep their
 * ordinary meaning. The cost is one extra copy per transaction.
 *
 * NO DESCRIPTOR PASSING. See the BINDER_TYPE_FD note in binder.h.
 */

#ifndef _FS_DEVFS_BINDER_INTERNAL_H_
#define _FS_DEVFS_BINDER_INTERNAL_H_

#include <fs/devfs/binder.h>

#include <kern/locks.h>
#include <libkern/OSMalloc.h>
#include <sys/kernel_types.h>
#include <sys/queue.h>
#include <sys/select.h>
#include <sys/types.h>

#pragma mark -
#pragma mark Limits

/* The lock group name, derived from the bundle id as the siblings do. */
#define DEVFS_LCKGRP_NAME        BUNDLEID_S ".lckgrp"

#define BINDER_MAX_CONTEXTS      16     /* binder, hwbinder, vndbinder + binderfs */
#define BINDER_CONTEXT_NAME_MAX  (BINDERFS_MAX_NAME + 1)

/*
 * A transaction's payload is copied into kernel memory between send and
 * delivery. Linux's ceiling is the target's mapping size; ours is the
 * same, but a single payload is additionally capped so one caller cannot
 * pin an arena's worth of kernel memory per in-flight transaction.
 */
#define BINDER_MAX_TRANSACTION_SIZE (1u * 1024u * 1024u)

/* What Linux allows before it starts refusing oneway traffic: half the
 * arena, so a flood of asynchronous calls cannot starve synchronous ones. */
#define BINDER_ASYNC_FRACTION 2

#pragma mark -
#pragma mark Work Items

enum binder_work_type {
	BINDER_WORK_TRANSACTION = 1,
	BINDER_WORK_TRANSACTION_COMPLETE,
	BINDER_WORK_RETURN_ERROR,
	BINDER_WORK_NODE,
	BINDER_WORK_DEAD_BINDER,
	BINDER_WORK_DEAD_BINDER_AND_CLEAR,
	BINDER_WORK_CLEAR_DEATH_NOTIFICATION,
};

/*
 * The unit of everything queued to a thread or a process. Embedded in the
 * object it describes rather than allocated beside it, so dequeuing is a
 * container_of and never an allocation that can fail.
 */
struct binder_work {
	TAILQ_ENTRY(binder_work) entry;
	enum binder_work_type type;
	bool queued;
};

TAILQ_HEAD(binder_work_head, binder_work);

/* A BR_ERROR waiting to be handed to a thread. */
struct binder_error {
	struct binder_work work;
	uint32_t cmd;
};

#pragma mark -
#pragma mark Nodes and References

struct binder_proc;
struct binder_thread;
struct binder_ref;
struct binder_transaction;

/*
 * A local object: something this process implements and others may hold a
 * handle to. `ptr` and `cookie` are the owner's own pointers, opaque here
 * and used only to name the object back to it.
 */
struct binder_node {
	struct binder_work work;              /* BINDER_WORK_NODE: refcount news */
	LIST_ENTRY(binder_node) proc_entry;
	struct binder_proc *proc;             /* NULL once the owner is gone */
	binder_uintptr_t ptr;
	binder_uintptr_t cookie;
	LIST_HEAD(, binder_ref) refs;

	int internal_strong_refs;             /* strong refs held by other procs */
	int local_strong_refs;                /* strong refs held by the owner   */
	int local_weak_refs;
	int tmp_refs;                         /* held across a lock drop         */

	bool has_strong_ref;                  /* owner has been told to acquire  */
	bool has_weak_ref;
	bool pending_strong_ref;              /* told, awaiting BC_ACQUIRE_DONE  */
	bool pending_weak_ref;

	bool has_async_transaction;
	bool accept_fds;
	bool is_dead;
	uint32_t flags;                       /* flat_binder_object.flags        */

	struct binder_work_head async_todo;
	int debug_id;
};

/* A death notification a process asked for on a handle. */
struct binder_ref_death {
	struct binder_work work;
	binder_uintptr_t cookie;
};

/*
 * A handle: one process's name for another's node. `desc` is the number
 * userspace sees. Descriptors are allocated lowest-free-first, and
 * descriptor 0 belongs to the context manager, exactly as on Linux.
 */
struct binder_ref {
	LIST_ENTRY(binder_ref) node_entry;    /* on node->refs   */
	LIST_ENTRY(binder_ref) proc_entry;    /* on proc->refs, ordered by desc */
	struct binder_proc *proc;
	struct binder_node *node;
	uint32_t desc;
	int strong;
	int weak;
	struct binder_ref_death *death;
	int debug_id;
};

#pragma mark -
#pragma mark Buffers

/*
 * One allocation inside a process's registered arena. The list covers the
 * whole arena with no gaps: adjacent free buffers are coalesced, so a
 * first-fit walk is enough and there is no separate free list to keep in
 * step with this one.
 */
struct binder_buffer {
	TAILQ_ENTRY(binder_buffer) entry;     /* ordered by offset */
	uint64_t offset;                      /* from the arena base */
	uint64_t size;                        /* total, including padding */
	bool free;
	bool async;
	bool allow_user_free;

	uint64_t data_size;
	uint64_t offsets_size;
	uint64_t extra_buffers_size;

	struct binder_transaction *transaction;
	struct binder_node *target_node;

	/*
	 * The payload, held here from send until the receiving thread copies
	 * it out. NULL once delivered. See the header comment.
	 */
	void *kdata;
	size_t kdata_size;
	int debug_id;
};

TAILQ_HEAD(binder_buffer_head, binder_buffer);

#pragma mark -
#pragma mark Transactions

struct binder_transaction {
	struct binder_work work;
	int debug_id;

	struct binder_thread *from;           /* NULL for oneway and once dead */
	struct binder_transaction *from_parent;
	struct binder_proc *to_proc;
	struct binder_thread *to_thread;
	struct binder_transaction *to_parent;

	bool need_reply;
	struct binder_buffer *buffer;

	uint32_t code;
	uint32_t flags;
	pid_t sender_pid;
	uid_t sender_euid;
};

#pragma mark -
#pragma mark Threads

/* binder_thread.looper */
enum {
	BINDER_LOOPER_STATE_REGISTERED  = 0x01,
	BINDER_LOOPER_STATE_ENTERED     = 0x02,
	BINDER_LOOPER_STATE_EXITED      = 0x04,
	BINDER_LOOPER_STATE_INVALID     = 0x08,
	BINDER_LOOPER_STATE_WAITING     = 0x10,
	BINDER_LOOPER_STATE_NEED_RETURN = 0x20,
};

struct binder_thread {
	LIST_ENTRY(binder_thread) entry;
	struct binder_proc *proc;
	uint64_t tid;                         /* thread_tid(current_thread()) */
	int looper;
	bool is_dead;
	int tmp_ref;

	struct binder_transaction *transaction_stack;
	struct binder_work_head todo;
	struct binder_error return_error;
	struct binder_error reply_error;
	int debug_id;
};

#pragma mark -
#pragma mark Processes and Contexts

/*
 * A context is a namespace: its own manager, its own nodes and handles.
 * /dev/binder, /dev/hwbinder and /dev/vndbinder are three of them, and
 * binderfs mints more. Which one an open belongs to is the device minor.
 */
struct binder_context {
	char name[BINDER_CONTEXT_NAME_MAX];
	struct binder_node *mgr_node;
	uid_t mgr_uid;
	bool mgr_uid_valid;
	bool in_use;
	void *devfs_handle;
	int minor;
	LIST_HEAD(, binder_proc) procs;
};

/* One open() of a binder device. Android opens once per process. */
struct binder_proc {
	LIST_ENTRY(binder_proc) entry;
	pid_t pid;
	struct binder_context *context;

	LIST_HEAD(, binder_thread) threads;
	LIST_HEAD(, binder_node) nodes;
	LIST_HEAD(, binder_ref) refs;         /* ordered by desc */
	struct binder_work_head todo;

	int max_threads;
	int requested_threads;
	int requested_threads_started;
	int ready_threads;
	bool is_dead;
	int tmp_ref;

	/* The client-registered arena. */
	user_addr_t arena_addr;
	uint64_t arena_size;
	struct binder_buffer_head buffers;
	uint64_t free_async_space;

	struct selinfo selinfo;
	bool sel_recorded;
};

#pragma mark -
#pragma mark Globals

extern lck_grp_t *devfs_lck_grp;
extern lck_mtx_t *binder_lock;
extern OSMallocTag devfs_osmalloc_tag;
extern struct binder_context binder_contexts[BINDER_MAX_CONTEXTS];

/* Diagnostics, published as devfs.binder.* sysctls. */
extern int64_t binder_stat_procs;
extern int64_t binder_stat_threads;
extern int64_t binder_stat_nodes;
extern int64_t binder_stat_refs;
extern int64_t binder_stat_transactions;
extern int64_t binder_stat_arena_bytes;
extern int64_t binder_stat_kdata_bytes;
extern int64_t binder_stat_failed_transactions;

#pragma mark -
#pragma mark Memory and Locking Helpers

void *binder_alloc_mem(size_t size);
void  binder_free_mem(void *addr, size_t size);
int   binder_next_debug_id(void);

#define BINDER_LOCK()    lck_mtx_lock(binder_lock)
#define BINDER_UNLOCK()  lck_mtx_unlock(binder_lock)

/*
 * Sleep on `chan` until binder_wake() names it, dropping the driver lock
 * for the duration. PCATCH, because a binder read must return to userspace
 * when a signal arrives - libbinder's thread pool depends on it.
 * Returns 0 on a wakeup, EINTR/ERESTART on a signal.
 */
int  binder_wait(void *chan);
void binder_wake(void *chan);

#pragma mark -
#pragma mark binder_alloc.c

int   binder_arena_register(struct binder_proc *proc, uint64_t addr, uint64_t size);
void  binder_arena_teardown(struct binder_proc *proc);
struct binder_buffer *binder_buffer_alloc(struct binder_proc *proc,
    uint64_t data_size, uint64_t offsets_size, uint64_t extra_buffers_size,
    bool is_async);
void  binder_buffer_free(struct binder_proc *proc, struct binder_buffer *buf);
struct binder_buffer *binder_buffer_lookup(struct binder_proc *proc,
    binder_uintptr_t user_ptr);
user_addr_t binder_buffer_uaddr(struct binder_proc *proc,
    const struct binder_buffer *buf);

#pragma mark -
#pragma mark binder_node.c

struct binder_node *binder_node_new(struct binder_proc *proc,
    binder_uintptr_t ptr, binder_uintptr_t cookie, uint32_t flags);
struct binder_node *binder_node_find(struct binder_proc *proc,
    binder_uintptr_t ptr);
void binder_node_inc_tmpref(struct binder_node *node);
void binder_node_dec_tmpref(struct binder_node *node);
int  binder_node_update_refs(struct binder_node *node, bool strong, bool increment,
    bool internal, struct binder_thread *thread);
void binder_node_release(struct binder_node *node);

struct binder_ref *binder_ref_get_for_node(struct binder_proc *proc,
    struct binder_node *node);
struct binder_ref *binder_ref_find_by_desc(struct binder_proc *proc, uint32_t desc);
int  binder_ref_update(struct binder_ref *ref, bool strong, bool increment,
    struct binder_thread *thread);
void binder_ref_delete(struct binder_ref *ref);

int  binder_context_mgr_set(struct binder_proc *proc, binder_uintptr_t ptr,
    binder_uintptr_t cookie, uint32_t flags);

int  binder_death_request(struct binder_proc *proc, struct binder_thread *thread,
    uint32_t handle, binder_uintptr_t cookie);
int  binder_death_clear(struct binder_proc *proc, struct binder_thread *thread,
    uint32_t handle, binder_uintptr_t cookie);
void binder_node_died(struct binder_node *node);

#pragma mark -
#pragma mark binder_txn.c

void binder_transaction(struct binder_proc *proc, struct binder_thread *thread,
    struct binder_transaction_data_sg *tr_sg, bool reply, bool is_sg);
void binder_free_transaction(struct binder_transaction *t);
void binder_transaction_buffer_release(struct binder_proc *proc,
    struct binder_buffer *buffer);

#pragma mark -
#pragma mark binder_proc.c

struct binder_proc   *binder_proc_new(struct binder_context *ctx);
void                  binder_proc_release(struct binder_proc *proc);
struct binder_thread *binder_thread_get(struct binder_proc *proc);
struct binder_thread *binder_thread_lookup(struct binder_proc *proc, uint64_t tid);
int                   binder_thread_release(struct binder_proc *proc,
                          struct binder_thread *thread);

void binder_enqueue_work(struct binder_proc *proc, struct binder_work *work);
void binder_enqueue_thread_work(struct binder_thread *thread, struct binder_work *work);
void binder_dequeue_work(struct binder_work *work);
bool binder_has_work(struct binder_proc *proc, struct binder_thread *thread);
void binder_wakeup_proc(struct binder_proc *proc);
void binder_wakeup_thread(struct binder_thread *thread);

int binder_ioctl_write_read(struct binder_proc *proc, struct binder_thread *thread,
    struct binder_write_read *bwr);
int binder_thread_write(struct binder_proc *proc, struct binder_thread *thread,
    user_addr_t buffer, uint64_t size, uint64_t *consumed);
int binder_thread_read(struct binder_proc *proc, struct binder_thread *thread,
    user_addr_t buffer, uint64_t size, uint64_t *consumed, bool non_block);

/* Put a BR_ command, and optionally a payload, into a read buffer. */
int binder_put_cmd(user_addr_t buffer, uint64_t size, uint64_t *consumed, uint32_t cmd);
int binder_put_cmd_data(user_addr_t buffer, uint64_t size, uint64_t *consumed,
    uint32_t cmd, const void *data, size_t len);

#pragma mark -
#pragma mark binder_dev.c

int  binder_devices_init(int major);
void binder_devices_fini(void);
struct binder_context *binder_context_for_minor(int minor);

#endif /* _FS_DEVFS_BINDER_INTERNAL_H_ */
