/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder_alloc.c
 *
 * The transaction arena: where a payload sits between the process that
 * sent it and the process that reads it.
 *
 * On Linux this file's counterpart owns memory the driver itself mapped
 * into the receiving process at mmap() time, and a transaction is copied
 * once, straight from the sender's parcel into that mapping. macOS does
 * not offer that: mmap() of a character device is refused outright, in
 * kern_mman.c, before any driver is consulted. So the shape here is
 * inverted - the client allocates its own region and tells the driver
 * about it (BINDER_MSL_SET_ARENA), the driver keeps the *bookkeeping* for
 * that region, and the payload waits in kernel memory until the receiving
 * thread is in the driver and a copyout to its address space is legal.
 *
 * Everything userspace can observe is unchanged: it is handed an address
 * inside its own region, that address is stable until it says
 * BC_FREE_BUFFER, and the driver refuses to hand out the same bytes twice.
 * What it costs is the second copy, and a rule the Linux driver does not
 * need - a payload is capped (BINDER_MAX_TRANSACTION_SIZE) so a sender
 * cannot make the kernel hold an arena's worth of memory per transaction.
 *
 * The free-list is the whole arena: every byte belongs to exactly one
 * binder_buffer, free or not, ordered by offset. Freeing coalesces with
 * both neighbours, so first-fit never fragments worse than the traffic
 * itself does, and there is no second list to keep in step with this one.
 */

#include <fs/devfs/binder_internal.h>

#include <kern/locks.h>
#include <libkern/libkern.h>
#include <libkext.h>
#include <sys/errno.h>
#include <sys/proc.h>

#pragma mark -
#pragma mark Memory and Waiting

/*
 * All driver memory comes from one tag, so OSMalloc_Tagfree at unload
 * reports anything leaked rather than leaving it to be found later. The
 * size travels with every free because OSFree needs it - the same
 * discipline the sysfs and procfs siblings keep.
 */
void *
binder_alloc_mem(size_t size)
{
	void *p;

	if (size == 0) {
		return NULL;
	}
	p = OSMalloc((uint32_t)size, devfs_osmalloc_tag);
	if (p != NULL) {
		bzero(p, size);
	}
	return p;
}

void
binder_free_mem(void *addr, size_t size)
{
	if (addr != NULL && size > 0) {
		OSFree(addr, (uint32_t)size, devfs_osmalloc_tag);
	}
}

/*
 * Sleep until somebody names this channel, with the driver lock dropped
 * for the duration and reacquired before returning - which is msleep's
 * ordinary contract, and the reason not to pass PDROP here.
 *
 * PCATCH is the point: a thread parked in BINDER_WRITE_READ must come back
 * to userspace when a signal arrives, because that is how libbinder's
 * thread pool is wound down and how a guest's own signals reach it. msleep
 * answers EINTR or ERESTART in that case and the read loop turns it into
 * the errno it reports.
 */
int
binder_wait(void *chan)
{
	return msleep(chan, binder_lock, PCATCH, "binder", NULL);
}

void
binder_wake(void *chan)
{
	wakeup(chan);
}

#pragma mark -
#pragma mark Debug Ids

/*
 * A monotonic id per object, so a log line or a sysctl can be matched to
 * the thing it describes. Wrapping is harmless.
 */
static int g_binder_last_debug_id = 0;

int
binder_next_debug_id(void)
{
	return ++g_binder_last_debug_id;
}

#pragma mark -
#pragma mark Arena Registration

#define BINDER_ALIGN(x) (((uint64_t)(x) + 7u) & ~7ull)

/* The smallest split worth making: less than this is left as slack on the
 * allocation rather than tracked as a free buffer of its own. */
#define BINDER_MIN_SPLIT 32u

/*
 * Take the client's word for a region of its address space, having checked
 * everything checkable without touching it. The first copyout into it is
 * what proves it is really there; if the client lied, or unmaps it later,
 * the copyout fails and the transaction is answered with BR_FAILED_REPLY
 * rather than the driver faulting.
 */
int
binder_arena_register(struct binder_proc *proc, uint64_t addr, uint64_t size)
{
	struct binder_buffer *buf;

	if (proc->arena_size != 0) {
		/* Linux refuses a second mmap of the same binder fd; so do we. */
		return EBUSY;
	}
	if (size < BINDER_MSL_ARENA_MIN || size > BINDER_MSL_ARENA_MAX) {
		return EINVAL;
	}
	if ((size & (PAGE_SIZE - 1)) != 0 || (addr & (PAGE_SIZE - 1)) != 0) {
		return EINVAL;
	}
	if (addr == 0 || addr + size < addr) {
		return EINVAL;
	}

	buf = binder_alloc_mem(sizeof(*buf));
	if (buf == NULL) {
		return ENOMEM;
	}
	buf->offset = 0;
	buf->size = size;
	buf->free = true;
	buf->debug_id = binder_next_debug_id();

	TAILQ_INIT(&proc->buffers);
	TAILQ_INSERT_HEAD(&proc->buffers, buf, entry);

	proc->arena_addr = (user_addr_t)addr;
	proc->arena_size = size;
	proc->free_async_space = size / BINDER_ASYNC_FRACTION;

	binder_stat_arena_bytes += (int64_t)size;
	return 0;
}

void
binder_arena_teardown(struct binder_proc *proc)
{
	struct binder_buffer *buf, *tmp;

	TAILQ_FOREACH_SAFE(buf, &proc->buffers, entry, tmp) {
		TAILQ_REMOVE(&proc->buffers, buf, entry);
		if (buf->kdata != NULL) {
			binder_free_mem(buf->kdata, buf->kdata_size);
			binder_stat_kdata_bytes -= (int64_t)buf->kdata_size;
		}
		binder_free_mem(buf, sizeof(*buf));
	}
	if (proc->arena_size != 0) {
		binder_stat_arena_bytes -= (int64_t)proc->arena_size;
	}
	proc->arena_addr = 0;
	proc->arena_size = 0;
	proc->free_async_space = 0;
}

#pragma mark -
#pragma mark Allocation

user_addr_t
binder_buffer_uaddr(struct binder_proc *proc, const struct binder_buffer *buf)
{
	return proc->arena_addr + buf->offset;
}

/*
 * First fit. The list is ordered by offset and covers the arena, so the
 * first free buffer large enough is taken and, if the remainder is worth
 * tracking, split off behind it.
 *
 * `is_async` draws against the oneway half of the arena: Linux's rule,
 * kept because it is what stops a process that only sends asynchronous
 * calls from denying service to everything synchronous.
 */
struct binder_buffer *
binder_buffer_alloc(struct binder_proc *proc, uint64_t data_size,
    uint64_t offsets_size, uint64_t extra_buffers_size, bool is_async)
{
	struct binder_buffer *buf, *split;
	uint64_t need;

	if (proc->arena_size == 0) {
		return NULL;   /* the client never registered an arena */
	}

	need = BINDER_ALIGN(data_size) + BINDER_ALIGN(offsets_size) +
	    BINDER_ALIGN(extra_buffers_size);
	if (need == 0 || need > proc->arena_size) {
		return NULL;
	}
	if (is_async && need > proc->free_async_space) {
		return NULL;
	}

	TAILQ_FOREACH(buf, &proc->buffers, entry) {
		if (buf->free && buf->size >= need) {
			break;
		}
	}
	if (buf == NULL) {
		return NULL;
	}

	if (buf->size - need >= BINDER_MIN_SPLIT) {
		split = binder_alloc_mem(sizeof(*split));
		if (split == NULL) {
			return NULL;
		}
		split->offset = buf->offset + need;
		split->size = buf->size - need;
		split->free = true;
		split->debug_id = binder_next_debug_id();
		TAILQ_INSERT_AFTER(&proc->buffers, buf, split, entry);
		buf->size = need;
	}

	buf->free = false;
	buf->async = is_async;
	buf->allow_user_free = false;
	buf->data_size = data_size;
	buf->offsets_size = offsets_size;
	buf->extra_buffers_size = extra_buffers_size;
	buf->transaction = NULL;
	buf->target_node = NULL;
	buf->kdata = NULL;
	buf->kdata_size = 0;

	if (is_async) {
		proc->free_async_space -= need;
	}
	return buf;
}

/*
 * Give the bytes back and merge with whichever neighbours are also free,
 * so the arena does not accumulate unusable slivers.
 */
void
binder_buffer_free(struct binder_proc *proc, struct binder_buffer *buf)
{
	struct binder_buffer *prev, *next;

	if (buf == NULL || buf->free) {
		return;
	}

	if (buf->kdata != NULL) {
		binder_free_mem(buf->kdata, buf->kdata_size);
		binder_stat_kdata_bytes -= (int64_t)buf->kdata_size;
		buf->kdata = NULL;
		buf->kdata_size = 0;
	}
	if (buf->async) {
		proc->free_async_space += buf->size;
	}

	buf->free = true;
	buf->async = false;
	buf->allow_user_free = false;
	buf->transaction = NULL;
	buf->target_node = NULL;
	buf->data_size = 0;
	buf->offsets_size = 0;
	buf->extra_buffers_size = 0;

	next = TAILQ_NEXT(buf, entry);
	if (next != NULL && next->free) {
		buf->size += next->size;
		TAILQ_REMOVE(&proc->buffers, next, entry);
		binder_free_mem(next, sizeof(*next));
	}
	prev = TAILQ_PREV(buf, binder_buffer_head, entry);
	if (prev != NULL && prev->free) {
		prev->size += buf->size;
		TAILQ_REMOVE(&proc->buffers, buf, entry);
		binder_free_mem(buf, sizeof(*buf));
	}
}

/*
 * Resolve the pointer a client hands back in BC_FREE_BUFFER. It must name
 * the start of a live allocation: an address inside one, or one already
 * freed, is a protocol error and is answered as such rather than guessed
 * at.
 */
struct binder_buffer *
binder_buffer_lookup(struct binder_proc *proc, binder_uintptr_t user_ptr)
{
	struct binder_buffer *buf;
	uint64_t offset;

	if (proc->arena_size == 0 || user_ptr < proc->arena_addr) {
		return NULL;
	}
	offset = user_ptr - proc->arena_addr;
	if (offset >= proc->arena_size) {
		return NULL;
	}

	TAILQ_FOREACH(buf, &proc->buffers, entry) {
		if (buf->offset == offset) {
			return buf->free ? NULL : buf;
		}
		if (buf->offset > offset) {
			break;
		}
	}
	return NULL;
}
