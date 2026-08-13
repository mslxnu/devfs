/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder_txn.c
 *
 * A transaction: taking a parcel from one process, rewriting every object
 * inside it so it means the same thing to another process, and queueing it
 * where that process will find it.
 *
 * The rewriting is the whole job. A parcel is bytes plus an array of
 * offsets naming where objects sit inside those bytes, and an object only
 * means something relative to the process holding it: the sender's local
 * object has to become the receiver's handle, the sender's handle has to
 * become either the receiver's own object (if the receiver is the owner)
 * or a handle of the receiver's own numbering. Getting that wrong does not
 * fail loudly - it hands one process a number that names something else
 * entirely - so every object is bounds-checked against the payload before
 * it is touched, and a payload that does not typecheck fails the whole
 * transaction rather than being partially translated.
 *
 * Two departures from Linux, both consequences of the arena model in
 * binder_alloc.c:
 *
 * - The payload is translated into kernel memory and copied out later, in
 *   the receiving thread. So the pointers written into it here are the
 *   addresses the *receiver* will see, computed from its arena base, and
 *   nothing in this file ever writes to a user address.
 *
 * - That kernel copy is kept until BC_FREE_BUFFER rather than freed after
 *   delivery, because releasing a buffer means walking its objects again
 *   to drop the references this file took. Re-reading them from the
 *   receiver's memory would mean trusting a client not to have scribbled
 *   over its own parcel - Linux can trust it, since the mapping is
 *   read-only to userspace, and here it is not.
 */

#include <fs/devfs/binder_internal.h>

#include <libkern/libkern.h>
#include <libkext.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/systm.h>

#define BINDER_ALIGN(x) (((uint64_t)(x) + 7u) & ~7ull)

#pragma mark -
#pragma mark Errors

/*
 * Fail the caller's transaction. The error is queued to the sending thread
 * rather than returned from the ioctl, because that is where userspace
 * looks for it: libbinder reads BR_FAILED_REPLY out of the read buffer and
 * turns it into a Status, and an errno from ioctl() would be reported as a
 * driver fault instead.
 */
static void
binder_txn_error(struct binder_thread *thread, uint32_t br)
{
	binder_stat_failed_transactions++;
	thread->return_error.cmd = br;
	thread->return_error.work.type = BINDER_WORK_RETURN_ERROR;
	binder_enqueue_thread_work(thread, &thread->return_error.work);
}

#pragma mark -
#pragma mark Object Walking

/* How many bytes an object of this type occupies inside a parcel. */
static size_t
binder_object_size(uint32_t type)
{
	switch (type) {
	case BINDER_TYPE_BINDER:
	case BINDER_TYPE_WEAK_BINDER:
	case BINDER_TYPE_HANDLE:
	case BINDER_TYPE_WEAK_HANDLE:
		return sizeof(struct flat_binder_object);
	case BINDER_TYPE_FD:
		return sizeof(struct binder_fd_object);
	case BINDER_TYPE_PTR:
		return sizeof(struct binder_buffer_object);
	case BINDER_TYPE_FDA:
		return sizeof(struct binder_fd_array_object);
	default:
		return 0;
	}
}

/*
 * Check that an offset names a whole, aligned object inside the data area,
 * and return a pointer to it in the kernel copy. NULL means the parcel is
 * malformed, which is always fatal to the transaction.
 */
static void *
binder_object_at(void *kdata, uint64_t data_size, uint64_t offset, size_t *sizep)
{
	struct binder_object_header *hdr;
	size_t objsize;

	if ((offset & 7) != 0 || offset >= data_size) {
		return NULL;
	}
	if (data_size - offset < sizeof(*hdr)) {
		return NULL;
	}
	hdr = (struct binder_object_header *)((char *)kdata + offset);
	objsize = binder_object_size(hdr->type);
	if (objsize == 0 || data_size - offset < objsize) {
		return NULL;
	}
	if (sizep != NULL) {
		*sizep = objsize;
	}
	return hdr;
}

#pragma mark -
#pragma mark Object Translation

/*
 * The sender is handing over one of its own objects. It becomes a handle
 * in the receiver, and the sender's node gains a reference held by that
 * handle - which is what keeps the object alive after the sender itself
 * lets go.
 */
static int
binder_translate_binder(struct flat_binder_object *fp, struct binder_proc *proc,
    struct binder_thread *thread, struct binder_proc *target_proc)
{
	struct binder_node *node;
	struct binder_ref *ref;
	int ret;

	node = binder_node_find(proc, fp->binder);
	if (node == NULL) {
		node = binder_node_new(proc, fp->binder, fp->cookie, fp->flags);
		if (node == NULL) {
			return ENOMEM;
		}
	}
	if (fp->cookie != node->cookie) {
		/* The same address described two ways: the sender is confused. */
		return EINVAL;
	}

	ref = binder_ref_get_for_node(target_proc, node);
	if (ref == NULL) {
		return ENOMEM;
	}
	ret = binder_ref_update(ref, fp->hdr.type == BINDER_TYPE_BINDER, true, thread);
	if (ret != 0) {
		return ret;
	}

	fp->hdr.type = (fp->hdr.type == BINDER_TYPE_BINDER) ?
	    BINDER_TYPE_HANDLE : BINDER_TYPE_WEAK_HANDLE;
	fp->binder = 0;
	fp->handle = ref->desc;
	fp->cookie = 0;
	return 0;
}

/*
 * The sender is passing on a handle. If the receiver happens to be the
 * process that owns the object, the handle collapses back into the object
 * itself - that is what makes a round trip through a third process return
 * a usable local pointer rather than a handle to yourself.
 */
static int
binder_translate_handle(struct flat_binder_object *fp, struct binder_proc *proc,
    struct binder_thread *thread, struct binder_proc *target_proc)
{
	struct binder_ref *ref, *new_ref;
	struct binder_node *node;
	bool strong = (fp->hdr.type == BINDER_TYPE_HANDLE);
	int ret;

	ref = binder_ref_find_by_desc(proc, fp->handle);
	if (ref == NULL || ref->node == NULL) {
		return EINVAL;
	}
	node = ref->node;

	if (node->proc == target_proc && node->proc != NULL) {
		ret = binder_node_update_refs(node, strong, true, false, thread);
		if (ret != 0) {
			return ret;
		}
		fp->hdr.type = strong ? BINDER_TYPE_BINDER : BINDER_TYPE_WEAK_BINDER;
		fp->binder = node->ptr;
		fp->cookie = node->cookie;
		return 0;
	}

	new_ref = binder_ref_get_for_node(target_proc, node);
	if (new_ref == NULL) {
		return ENOMEM;
	}
	ret = binder_ref_update(new_ref, strong, true, thread);
	if (ret != 0) {
		return ret;
	}
	fp->handle = new_ref->desc;
	return 0;
}

/*
 * A descriptor. The kernel cannot move one - see the note at the top of
 * binder.h - so the object is validated, the sender's descriptor number is
 * left in place, and the sender's pid goes into the cookie, which Linux
 * leaves as padding. A userspace broker turns that pair into a real
 * descriptor on the far side.
 */
static int
binder_translate_fd(struct binder_fd_object *fp, struct binder_proc *proc,
    struct binder_node *target_node)
{
	if (target_node != NULL && !target_node->accept_fds) {
		return EPERM;   /* the receiver said it does not take descriptors */
	}
	fp->cookie = (binder_uintptr_t)proc->pid;
	return 0;
}

#pragma mark -
#pragma mark Transactions

/*
 * Release a transaction, having first removed every reference to it that
 * would outlive it.
 *
 * A synchronous transaction is on two thread stacks at once - the caller's,
 * which is waiting for the reply, and the server's, which owes it - and
 * either can be the one that frees it. Popping only the stack the caller of
 * this function happens to be holding leaves the other thread with a
 * dangling pointer that is not dereferenced until its *next* transaction or
 * its close, which is a long way from the cause. Both are popped here,
 * where it cannot be forgotten, as Linux does in
 * binder_pop_transaction_ilocked().
 */
void
binder_free_transaction(struct binder_transaction *t)
{
	if (t == NULL) {
		return;
	}

	/* Never free something still linked into a queue. */
	binder_dequeue_work(&t->work);

	if (t->from != NULL && t->from->transaction_stack == t) {
		t->from->transaction_stack = t->from_parent;
	}
	if (t->to_thread != NULL && t->to_thread->transaction_stack == t) {
		t->to_thread->transaction_stack = t->to_parent;
	}
	if (t->buffer != NULL) {
		t->buffer->transaction = NULL;
	}
	binder_free_mem(t, sizeof(*t));
	binder_stat_transactions--;
}

/*
 * Undo, one object at a time, everything binder_transaction() did to the
 * parcel now being freed. Called from BC_FREE_BUFFER and from teardown,
 * and it walks the kernel copy rather than the receiver's memory for the
 * reason given in the file comment.
 */
void
binder_transaction_buffer_release(struct binder_proc *proc,
    struct binder_buffer *buffer)
{
	binder_size_t *offsets;
	uint64_t count, i;
	void *kdata;

	if (buffer == NULL || buffer->kdata == NULL) {
		return;
	}
	kdata = buffer->kdata;
	offsets = (binder_size_t *)((char *)kdata + BINDER_ALIGN(buffer->data_size));
	count = buffer->offsets_size / sizeof(binder_size_t);

	for (i = 0; i < count; i++) {
		struct binder_object_header *hdr;
		size_t objsize;

		hdr = binder_object_at(kdata, buffer->data_size, offsets[i], &objsize);
		if (hdr == NULL) {
			continue;   /* validated on the way in; nothing to undo */
		}

		switch (hdr->type) {
		case BINDER_TYPE_BINDER:
		case BINDER_TYPE_WEAK_BINDER: {
			struct flat_binder_object *fp = (struct flat_binder_object *)hdr;
			struct binder_node *node = binder_node_find(proc, fp->binder);

			if (node != NULL) {
				binder_node_update_refs(node,
				    hdr->type == BINDER_TYPE_BINDER, false, false, NULL);
			}
			break;
		}
		case BINDER_TYPE_HANDLE:
		case BINDER_TYPE_WEAK_HANDLE: {
			struct flat_binder_object *fp = (struct flat_binder_object *)hdr;
			struct binder_ref *ref = binder_ref_find_by_desc(proc, fp->handle);

			if (ref != NULL) {
				binder_ref_update(ref, hdr->type == BINDER_TYPE_HANDLE,
				    false, NULL);
			}
			break;
		}
		default:
			/* Descriptors and scatter-gather buffers hold no reference. */
			break;
		}
	}
}

/*
 * Which thread, if any, should receive this transaction directly. A nested
 * call - A calls B, B calls back into A while A is still waiting - must go
 * to the thread that is already waiting, or both sides deadlock waiting for
 * a free thread that the other is holding.
 */
static struct binder_thread *
binder_target_thread_for(struct binder_thread *thread, struct binder_proc *target_proc)
{
	struct binder_transaction *tmp = thread->transaction_stack;

	while (tmp != NULL) {
		if (tmp->from != NULL && tmp->from->proc == target_proc) {
			return tmp->from;
		}
		tmp = tmp->from_parent;
	}
	return NULL;
}

/*
 * Send a transaction or a reply.
 *
 * `tr_sg` is the client's binder_transaction_data, with the scatter-gather
 * size appended when the command was BC_TRANSACTION_SG/BC_REPLY_SG. Errors
 * are reported by queueing BR_FAILED_REPLY or BR_DEAD_REPLY to the caller,
 * never by leaving half a transaction behind.
 */
void
binder_transaction(struct binder_proc *proc, struct binder_thread *thread,
    struct binder_transaction_data_sg *tr_sg, bool reply, bool is_sg)
{
	struct binder_transaction_data *tr = &tr_sg->transaction_data;
	struct binder_transaction *t = NULL;
	struct binder_transaction *in_reply_to = NULL;
	struct binder_proc *target_proc = NULL;
	struct binder_thread *target_thread = NULL;
	struct binder_node *target_node = NULL;
	struct binder_buffer *buffer = NULL;
	struct binder_work *tcomplete = NULL;
	uint64_t extra_size = is_sg ? tr_sg->buffers_size : 0;
	uint64_t total = 0, off_area, sg_area, sg_used = 0;
	binder_size_t *offsets;
	uint64_t count, i;
	void *kdata = NULL;
	bool oneway = (tr->flags & TF_ONE_WAY) != 0;
	int error;

	if (reply) {
		in_reply_to = thread->transaction_stack;
		if (in_reply_to == NULL) {
			binder_txn_error(thread, BR_FAILED_REPLY);
			return;
		}
		thread->transaction_stack = in_reply_to->to_parent;
		target_thread = in_reply_to->from;
		if (target_thread == NULL || target_thread->is_dead) {
			binder_txn_error(thread, BR_DEAD_REPLY);
			binder_free_transaction(in_reply_to);
			return;
		}
		target_proc = target_thread->proc;
		oneway = false;
	} else {
		if (tr->target.handle != 0) {
			struct binder_ref *ref =
			    binder_ref_find_by_desc(proc, tr->target.handle);

			if (ref == NULL || ref->node == NULL) {
				binder_txn_error(thread, BR_FAILED_REPLY);
				return;
			}
			target_node = ref->node;
		} else {
			target_node = proc->context != NULL ?
			    proc->context->mgr_node : NULL;
			if (target_node == NULL) {
				/* Nobody has registered as the context manager yet. */
				binder_txn_error(thread, BR_DEAD_REPLY);
				return;
			}
		}
		if (target_node->is_dead || target_node->proc == NULL ||
		    target_node->proc->is_dead) {
			binder_txn_error(thread, BR_DEAD_REPLY);
			return;
		}
		target_proc = target_node->proc;
		binder_node_inc_tmpref(target_node);

		if (!oneway) {
			target_thread = binder_target_thread_for(thread, target_proc);
		}
	}

	if (target_proc->arena_size == 0) {
		/* The receiver never registered an arena, so it cannot be given
		 * a payload at all. Its own fault, but the sender's error. */
		binder_txn_error(thread, BR_FAILED_REPLY);
		goto out;
	}

	/* Bounds first: everything below trusts these three sizes. */
	if (tr->data_size > BINDER_MAX_TRANSACTION_SIZE ||
	    tr->offsets_size > BINDER_MAX_TRANSACTION_SIZE ||
	    extra_size > BINDER_MAX_TRANSACTION_SIZE ||
	    (tr->offsets_size % sizeof(binder_size_t)) != 0) {
		binder_txn_error(thread, BR_FAILED_REPLY);
		goto out;
	}

	off_area = BINDER_ALIGN(tr->data_size);
	sg_area = off_area + BINDER_ALIGN(tr->offsets_size);
	total = sg_area + BINDER_ALIGN(extra_size);
	if (total == 0 || total > BINDER_MAX_TRANSACTION_SIZE) {
		binder_txn_error(thread, BR_FAILED_REPLY);
		goto out;
	}

	buffer = binder_buffer_alloc(target_proc, tr->data_size, tr->offsets_size,
	    extra_size, oneway);
	if (buffer == NULL) {
		binder_txn_error(thread, BR_FAILED_REPLY);
		goto out;
	}

	kdata = binder_alloc_mem((size_t)total);
	if (kdata == NULL) {
		binder_txn_error(thread, BR_FAILED_REPLY);
		goto out;
	}

	if (copyin((user_addr_t)tr->data.ptr.buffer, kdata, (size_t)tr->data_size) != 0 ||
	    copyin((user_addr_t)tr->data.ptr.offsets, (char *)kdata + off_area,
	    (size_t)tr->offsets_size) != 0) {
		binder_txn_error(thread, BR_FAILED_REPLY);
		goto out;
	}

	buffer->kdata = kdata;
	buffer->kdata_size = (size_t)total;
	binder_stat_kdata_bytes += (int64_t)total;
	kdata = NULL;   /* the buffer owns it now */

	/* Translate every object named by the offsets array. */
	offsets = (binder_size_t *)((char *)buffer->kdata + off_area);
	count = tr->offsets_size / sizeof(binder_size_t);
	for (i = 0; i < count; i++) {
		struct binder_object_header *hdr;
		size_t objsize;

		hdr = binder_object_at(buffer->kdata, tr->data_size, offsets[i], &objsize);
		if (hdr == NULL) {
			binder_txn_error(thread, BR_FAILED_REPLY);
			goto out;
		}

		error = 0;
		switch (hdr->type) {
		case BINDER_TYPE_BINDER:
		case BINDER_TYPE_WEAK_BINDER:
			error = binder_translate_binder((struct flat_binder_object *)hdr,
			    proc, thread, target_proc);
			break;
		case BINDER_TYPE_HANDLE:
		case BINDER_TYPE_WEAK_HANDLE:
			error = binder_translate_handle((struct flat_binder_object *)hdr,
			    proc, thread, target_proc);
			break;
		case BINDER_TYPE_FD:
			error = binder_translate_fd((struct binder_fd_object *)hdr,
			    proc, target_node);
			break;
	case BINDER_TYPE_PTR: {
		struct binder_buffer_object *bp = (struct binder_buffer_object *)hdr;
		uint64_t need = BINDER_ALIGN(bp->length);
		void *src;
		uint64_t src_len;
		size_t j;

		if (bp->flags & BINDER_BUFFER_FLAG_HAS_PARENT) {
			struct binder_buffer_object *parent = NULL;

			for (j = 0; j < count; j++) {
				if (offsets[j] == bp->parent) {
					parent = (struct binder_buffer_object *)
					    (buffer->kdata + offsets[j]);
					break;
				}
			}
			if (parent == NULL) {
				printf("devfs: binder: nested buffer parent "
				    "not found (pid %d)\n", proc->pid);
				error = EINVAL;
				break;
			}
			if (bp->buffer < parent->buffer ||
			    bp->buffer + bp->length > parent->buffer + parent->length) {
				printf("devfs: binder: nested buffer outside "
				    "parent (pid %d)\n", proc->pid);
				error = EINVAL;
				break;
			}
			src = (void *)(uintptr_t)bp->buffer;
			src_len = bp->length;
		} else {
			src = (void *)(uintptr_t)bp->buffer;
			src_len = bp->length;
		}

		if (src_len > extra_size || sg_used + need > BINDER_ALIGN(extra_size)) {
			error = EINVAL;
			break;
		}
		if (copyin((user_addr_t)src,
		    (char *)buffer->kdata + sg_area + sg_used,
		    (size_t)src_len) != 0) {
			error = EFAULT;
			break;
		}
		bp->buffer = (binder_uintptr_t)(binder_buffer_uaddr(target_proc,
		    buffer) + sg_area + sg_used);
		sg_used += need;
		break;
	}
		case BINDER_TYPE_FDA:
			/*
			 * An array of descriptors. The kernel cannot move descriptors
			 * between processes, so the object is passed through
			 * unchanged: NABI's userspace broker translates the array
			 * over SCM_RIGHTS, keyed by the sender_pid the driver
			 * stamps into the transaction header.
			 */
			break;
		default:
			error = EINVAL;
			break;
		}

		if (error != 0) {
			binder_txn_error(thread, BR_FAILED_REPLY);
			goto out;
		}
	}

	t = binder_alloc_mem(sizeof(*t));
	tcomplete = binder_alloc_mem(sizeof(*tcomplete));
	if (t == NULL || tcomplete == NULL) {
		binder_txn_error(thread, BR_FAILED_REPLY);
		goto out;
	}

	t->debug_id = binder_next_debug_id();
	t->work.type = BINDER_WORK_TRANSACTION;
	/*
	 * Only a call that expects an answer records who sent it. A reply is
	 * never replied to and an asynchronous transaction is never waited on,
	 * so in both cases the pointer would only be a way for a thread that
	 * exits first to leave a dangling reference behind it.
	 */
	t->from = (reply || oneway) ? NULL : thread;
	t->to_proc = target_proc;
	t->to_thread = target_thread;
	t->code = tr->code;
	t->flags = tr->flags;
	t->sender_pid = proc->pid;
	/* The sender's effective uid, taken here because this runs in the
	 * sender's own context; the receiver reads it out of BR_TRANSACTION. */
	t->sender_euid = kauth_cred_getuid(kauth_cred_get());
	t->need_reply = !reply && !oneway;
	t->buffer = buffer;
	buffer->transaction = t;
	buffer->target_node = target_node;
	binder_stat_transactions++;

	tcomplete->type = BINDER_WORK_TRANSACTION_COMPLETE;

	if (reply) {
		binder_free_transaction(in_reply_to);
		in_reply_to = NULL;
		binder_enqueue_thread_work(target_thread, &t->work);
		binder_wakeup_thread(target_thread);
	} else if (!oneway) {
		t->from_parent = thread->transaction_stack;
		thread->transaction_stack = t;
		if (target_thread != NULL) {
			binder_enqueue_thread_work(target_thread, &t->work);
			binder_wakeup_thread(target_thread);
		} else {
			binder_enqueue_work(target_proc, &t->work);
			binder_wakeup_proc(target_proc);
		}
	} else {
		/*
		 * One asynchronous transaction per object at a time: the rest
		 * queue behind it on the node, so a flood aimed at one object
		 * cannot fill the receiver's todo list and starve everything
		 * else it serves.
		 */
		if (target_node->has_async_transaction) {
			TAILQ_INSERT_TAIL(&target_node->async_todo, &t->work, entry);
			t->work.queue = &target_node->async_todo;
		} else {
			target_node->has_async_transaction = true;
			binder_enqueue_work(target_proc, &t->work);
			binder_wakeup_proc(target_proc);
		}
	}

	binder_enqueue_thread_work(thread, tcomplete);
	tcomplete = NULL;
	buffer = NULL;
	t = NULL;

out:
	if (t != NULL) {
		binder_free_mem(t, sizeof(*t));
	}
	if (tcomplete != NULL) {
		binder_free_mem(tcomplete, sizeof(*tcomplete));
	}
	if (kdata != NULL) {
		binder_free_mem(kdata, (size_t)total);
	}
	if (buffer != NULL) {
		binder_transaction_buffer_release(target_proc, buffer);
		binder_buffer_free(target_proc, buffer);
	}
	if (in_reply_to != NULL) {
		/* The reply failed after the stack was popped: put it back so the
		 * caller can be told, and so the far side is not left waiting. */
		thread->transaction_stack = in_reply_to;
	}
	if (target_node != NULL) {
		binder_node_dec_tmpref(target_node);
	}
}
