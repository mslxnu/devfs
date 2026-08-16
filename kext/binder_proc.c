/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder_proc.c
 *
 * Processes, threads, the queues between them, and the two loops that are
 * the whole of BINDER_WRITE_READ: one that consumes the client's BC_
 * commands, one that produces BR_ commands for it to read.
 *
 * The thing worth understanding before changing anything here is where a
 * piece of work is allowed to sit. A transaction aimed at a process goes
 * on the process queue and any thread in its pool may take it; a
 * transaction that is a reply, or a nested call back into a thread that is
 * already waiting, goes on that thread's own queue and only it may take
 * it. Get that wrong in the direction of "any thread will do" and a nested
 * call deadlocks: the thread waiting for the reply is not the one that
 * picked it up. Get it wrong the other way and the pool never balances.
 *
 * A thread only takes process work when it has no transaction of its own
 * outstanding - which is what binder_has_work() encodes, and what
 * d_select() reuses so poll() and the read loop cannot disagree about
 * whether there is anything to do.
 *
 * Waiting is msleep with PCATCH on one of two channels: the process queue
 * for pool threads, the thread itself for directed work. Both are woken by
 * name, and the driver lock is dropped for the duration (binder_wait).
 */

#include <fs/devfs/binder_internal.h>

#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkext.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/select.h>

#define BINDER_ALIGN(x) (((uint64_t)(x) + 7u) & ~7ull)

/* container_of, spelled out rather than pulled from a kernel header that
 * does not export it to kexts. */
#define BINDER_CONTAINER_OF(ptr, type, member) \
	((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

#pragma mark -
#pragma mark Queues

static void
binder_queue_insert(struct binder_work_head *q, struct binder_work *work, bool head)
{
	if (work->queue != NULL) {
		return;         /* already waiting somewhere; leave it there */
	}
	if (head) {
		TAILQ_INSERT_HEAD(q, work, entry);
	} else {
		TAILQ_INSERT_TAIL(q, work, entry);
	}
	work->queue = q;
}

void
binder_enqueue_work(struct binder_proc *proc, struct binder_work *work)
{
	binder_queue_insert(&proc->todo, work, false);
}

void
binder_enqueue_thread_work(struct binder_thread *thread, struct binder_work *work)
{
	binder_queue_insert(&thread->todo, work, false);
}

/*
 * Take a work item off whichever queue holds it, wherever that is. Every
 * removal goes through here - including the ones that are about to free the
 * item - because an item freed while still linked corrupts the list it was
 * on, and the driver notices only when something later walks it.
 */
void
binder_dequeue_work(struct binder_work *work)
{
	if (work->queue == NULL) {
		return;
	}
	TAILQ_REMOVE(work->queue, work, entry);
	work->queue = NULL;
}

/*
 * May this thread take work aimed at the process as a whole? Only if it
 * has joined the pool and owes nobody a reply.
 */
static bool
binder_thread_takes_proc_work(struct binder_thread *thread)
{
	if (thread->transaction_stack != NULL) {
		return false;
	}
	if (!TAILQ_EMPTY(&thread->todo)) {
		return false;
	}
	return (thread->looper & (BINDER_LOOPER_STATE_REGISTERED |
	    BINDER_LOOPER_STATE_ENTERED)) != 0;
}

bool
binder_has_work(struct binder_proc *proc, struct binder_thread *thread)
{
	if (thread != NULL) {
		if (!TAILQ_EMPTY(&thread->todo)) {
			return true;
		}
		if (thread->looper & BINDER_LOOPER_STATE_NEED_RETURN) {
			return true;
		}
		if (!binder_thread_takes_proc_work(thread)) {
			return false;
		}
	}
	return !TAILQ_EMPTY(&proc->todo);
}

void
binder_wakeup_proc(struct binder_proc *proc)
{
	binder_wake(&proc->todo);
	if (proc->sel_recorded) {
		selwakeup(&proc->selinfo);
		proc->sel_recorded = false;
	}
}

void
binder_wakeup_thread(struct binder_thread *thread)
{
	binder_wake(thread);
	if (thread->proc != NULL && thread->proc->sel_recorded) {
		selwakeup(&thread->proc->selinfo);
		thread->proc->sel_recorded = false;
	}
}

#pragma mark -
#pragma mark Threads

struct binder_thread *
binder_thread_lookup(struct binder_proc *proc, uint64_t tid)
{
	struct binder_thread *thread;

	LIST_FOREACH(thread, &proc->threads, entry) {
		if (thread->tid == tid) {
			return thread;
		}
	}
	return NULL;
}

/*
 * The calling thread's binder state, created on first use. Threads are
 * identified by thread_tid(), which is stable for the life of the thread
 * and unique machine-wide - unlike a pid, which every thread in a process
 * shares.
 */
struct binder_thread *
binder_thread_get(struct binder_proc *proc)
{
	struct binder_thread *thread;
	uint64_t tid = thread_tid(current_thread());

	thread = binder_thread_lookup(proc, tid);
	if (thread != NULL) {
		return thread;
	}

	thread = binder_alloc_mem(sizeof(*thread));
	if (thread == NULL) {
		return NULL;
	}
	thread->proc = proc;
	thread->tid = tid;
	thread->debug_id = binder_next_debug_id();
	thread->return_error.cmd = BR_OK;
	thread->reply_error.cmd = BR_OK;
	TAILQ_INIT(&thread->todo);
	LIST_INSERT_HEAD(&proc->threads, thread, entry);
	binder_stat_threads++;
	return thread;
}

/*
 * BINDER_THREAD_EXIT, or the process going away. Anything this thread was
 * in the middle of has to be answered, or the far side waits forever.
 */
int
binder_thread_release(struct binder_proc *proc, struct binder_thread *thread)
{
	struct binder_transaction *t;
	struct binder_work *w, *tmp;

	thread->is_dead = true;

	/*
	 * Every transaction still on this thread's stack has somebody waiting
	 * on it. A transaction we received owes its sender a reply, which will
	 * now never come, so the sender is told the receiver died; one we sent
	 * is detached so its reply is dropped rather than delivered to freed
	 * memory.
	 */
	t = thread->transaction_stack;
	while (t != NULL) {
		struct binder_transaction *next;

		if (t->from == thread) {
			next = t->from_parent;
			t->from = NULL;
		} else if (t->to_thread == thread) {
			next = t->to_parent;
			t->to_thread = NULL;
			/*
			 * The payload was allocated in this process's arena and
			 * that arena is going away, so it is released here rather
			 * than left for the sender to free a buffer that no longer
			 * exists.
			 */
			if (t->buffer != NULL) {
				binder_transaction_buffer_release(proc, t->buffer);
				binder_buffer_free(proc, t->buffer);
				t->buffer = NULL;
			}
			if (t->from != NULL && !t->from->is_dead) {
				t->from->return_error.cmd = BR_DEAD_REPLY;
				t->from->return_error.work.type = BINDER_WORK_RETURN_ERROR;
				binder_enqueue_thread_work(t->from,
				    &t->from->return_error.work);
				binder_wakeup_thread(t->from);
			}
		} else {
			next = NULL;
		}
		t = next;
	}
	thread->transaction_stack = NULL;

	TAILQ_FOREACH_SAFE(w, &thread->todo, entry, tmp) {
		binder_dequeue_work(w);
		if (w->type == BINDER_WORK_TRANSACTION) {
			struct binder_transaction *tr =
			    BINDER_CONTAINER_OF(w, struct binder_transaction, work);

			/* Undelivered: hand it back to the process queue so another
			 * thread can take it, unless the process itself is going. */
			if (!proc->is_dead) {
				binder_enqueue_work(proc, w);
			} else {
				if (tr->buffer != NULL) {
					binder_transaction_buffer_release(proc, tr->buffer);
					binder_buffer_free(proc, tr->buffer);
				}
				binder_free_transaction(tr);
			}
		} else if (w->type == BINDER_WORK_TRANSACTION_COMPLETE) {
			binder_free_mem(w, sizeof(*w));
		}
	}

	LIST_REMOVE(thread, entry);
	binder_free_mem(thread, sizeof(*thread));
	binder_stat_threads--;
	return 0;
}

#pragma mark -
#pragma mark Processes

struct binder_proc *
binder_proc_new(struct binder_context *ctx)
{
	struct binder_proc *proc;

	proc = binder_alloc_mem(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->pid = proc_selfpid();
	proc->context = ctx;
	proc->max_threads = 15;   /* what libbinder asks for; raised by ioctl */
	LIST_INIT(&proc->threads);
	LIST_INIT(&proc->nodes);
	LIST_INIT(&proc->refs);
	TAILQ_INIT(&proc->todo);
	TAILQ_INIT(&proc->buffers);

	LIST_INSERT_HEAD(&ctx->procs, proc, entry);
	binder_stat_procs++;
	return proc;
}

/*
 * The device was closed. Everything this process owned has to be undone in
 * an order that leaves nobody waiting: first tell the world its objects
 * are dead, then drop the handles it held, then answer whatever it was in
 * the middle of, then release the memory.
 */
void
binder_proc_release(struct binder_proc *proc)
{
	struct binder_thread *thread, *tthread;
	struct binder_node *node, *tnode;
	struct binder_ref *ref, *tref;
	struct binder_work *w, *tw;

	proc->is_dead = true;

	/*
	 * Give up the context manager role first, while the node still knows
	 * which process owned it: binder_node_died() below clears node->proc,
	 * and a check made after that can never recognise its own manager -
	 * which left the role held by a dead process for the life of the
	 * kext, so the next servicemanager to start got EBUSY forever.
	 *
	 * The two self-references taken in binder_context_mgr_set() go with
	 * it. They exist to keep the node alive while the role is held, so
	 * releasing the role has to release them too or the node outlives
	 * every reason to exist. Linux clears the same field on the same
	 * path, in binder_deferred_release().
	 */
	if (proc->context != NULL && proc->context->mgr_node != NULL &&
	    proc->context->mgr_node->proc == proc) {
		struct binder_node *mgr = proc->context->mgr_node;

		proc->context->mgr_node = NULL;
		mgr->local_strong_refs = 0;
		mgr->local_weak_refs = 0;
		mgr->has_strong_ref = false;
		mgr->has_weak_ref = false;
	}

	/* Objects this process implemented are now dead; anybody holding a
	 * handle with a death notification hears about it here. */
	LIST_FOREACH_SAFE(node, &proc->nodes, proc_entry, tnode) {
		LIST_REMOVE(node, proc_entry);
		binder_node_died(node);
		binder_node_release(node);
	}

	/* Handles this process held. Dropping them may be the last reference
	 * to somebody else's object. */
	LIST_FOREACH_SAFE(ref, &proc->refs, proc_entry, tref) {
		binder_ref_delete(ref);
	}

	/* Work nobody will now collect. Transactions still owe their senders
	 * an answer. */
	TAILQ_FOREACH_SAFE(w, &proc->todo, entry, tw) {
		binder_dequeue_work(w);
		switch (w->type) {
		case BINDER_WORK_TRANSACTION: {
			struct binder_transaction *t =
			    BINDER_CONTAINER_OF(w, struct binder_transaction, work);

			if (t->from != NULL && !t->from->is_dead) {
				t->from->return_error.cmd = BR_DEAD_REPLY;
				t->from->return_error.work.type = BINDER_WORK_RETURN_ERROR;
				binder_enqueue_thread_work(t->from,
				    &t->from->return_error.work);
				binder_wakeup_thread(t->from);
			}
			if (t->buffer != NULL) {
				binder_transaction_buffer_release(proc, t->buffer);
				binder_buffer_free(proc, t->buffer);
			}
			binder_free_transaction(t);
			break;
		}
		case BINDER_WORK_TRANSACTION_COMPLETE:
			binder_free_mem(w, sizeof(*w));
			break;
		default:
			break;
		}
	}

	LIST_FOREACH_SAFE(thread, &proc->threads, entry, tthread) {
		binder_wakeup_thread(thread);
		binder_thread_release(proc, thread);
	}

	binder_arena_teardown(proc);
	if (proc->sel_recorded) {
		selthreadclear(&proc->selinfo);
		proc->sel_recorded = false;
	}
	LIST_REMOVE(proc, entry);
	binder_free_mem(proc, sizeof(*proc));
	binder_stat_procs--;
}

#pragma mark -
#pragma mark Read-Buffer Helpers

int
binder_put_cmd(user_addr_t buffer, uint64_t size, uint64_t *consumed, uint32_t cmd)
{
	if (size - *consumed < sizeof(uint32_t)) {
		return ENOMEM;
	}
	if (copyout(&cmd, buffer + *consumed, sizeof(cmd)) != 0) {
		return EFAULT;
	}
	*consumed += sizeof(uint32_t);
	return 0;
}

int
binder_put_cmd_data(user_addr_t buffer, uint64_t size, uint64_t *consumed,
    uint32_t cmd, const void *data, size_t len)
{
	if (size - *consumed < sizeof(uint32_t) + len) {
		return ENOMEM;
	}
	if (copyout(&cmd, buffer + *consumed, sizeof(cmd)) != 0) {
		return EFAULT;
	}
	if (copyout(data, buffer + *consumed + sizeof(cmd), len) != 0) {
		return EFAULT;
	}
	*consumed += sizeof(uint32_t) + len;
	return 0;
}

#pragma mark -
#pragma mark The Write Loop

/* Read one item out of the client's write buffer. */
static int
binder_get_user(user_addr_t buffer, uint64_t *consumed, void *dst, size_t len)
{
	if (copyin(buffer + *consumed, dst, len) != 0) {
		return EFAULT;
	}
	*consumed += len;
	return 0;
}

/*
 * A reference command naming a handle. Handle 0 is special on the way up:
 * a process that has never spoken to the context manager has no reference
 * to it, and asking for one is how it bootstraps.
 */
static int
binder_write_ref_cmd(struct binder_proc *proc, struct binder_thread *thread,
    uint32_t cmd, uint32_t desc)
{
	struct binder_ref *ref;
	bool strong = (cmd == BC_ACQUIRE || cmd == BC_RELEASE);
	bool increment = (cmd == BC_INCREFS || cmd == BC_ACQUIRE);

	ref = binder_ref_find_by_desc(proc, desc);
	if (ref == NULL) {
		if (!increment || desc != 0 || proc->context == NULL ||
		    proc->context->mgr_node == NULL) {
			return EINVAL;
		}
		ref = binder_ref_get_for_node(proc, proc->context->mgr_node);
		if (ref == NULL) {
			return ENOMEM;
		}
	}
	return binder_ref_update(ref, strong, increment, thread);
}

/*
 * Release a buffer the client is done with, and let the next asynchronous
 * transaction aimed at the same object through - the one-at-a-time rule
 * from binder_txn.c is only fair if the queue drains here.
 */
static int
binder_write_free_buffer(struct binder_proc *proc, binder_uintptr_t data_ptr)
{
	struct binder_buffer *buffer;
	struct binder_node *node;

	buffer = binder_buffer_lookup(proc, data_ptr);
	if (buffer == NULL || !buffer->allow_user_free) {
		return EINVAL;
	}
	node = buffer->target_node;

	binder_transaction_buffer_release(proc, buffer);
	if (buffer->transaction != NULL) {
		buffer->transaction->buffer = NULL;
		buffer->transaction = NULL;
	}
	binder_buffer_free(proc, buffer);

	if (node != NULL && node->has_async_transaction) {
		struct binder_work *next = TAILQ_FIRST(&node->async_todo);

		if (next != NULL) {
			binder_dequeue_work(next);
			binder_enqueue_work(proc, next);
			binder_wakeup_proc(proc);
		} else {
			node->has_async_transaction = false;
		}
	}
	return 0;
}

int
binder_thread_write(struct binder_proc *proc, struct binder_thread *thread,
    user_addr_t buffer, uint64_t size, uint64_t *consumed)
{
	int ret;

	while (*consumed + sizeof(uint32_t) <= size) {
		uint32_t cmd;

		if (binder_get_user(buffer, consumed, &cmd, sizeof(cmd)) != 0) {
			return EFAULT;
		}

		switch (cmd) {
		case BC_INCREFS:
		case BC_ACQUIRE:
		case BC_RELEASE:
		case BC_DECREFS: {
			uint32_t desc;

			if (binder_get_user(buffer, consumed, &desc, sizeof(desc)) != 0) {
				return EFAULT;
			}
			ret = binder_write_ref_cmd(proc, thread, cmd, desc);
			if (ret != 0) {
				return ret;
			}
			break;
		}
		case BC_INCREFS_DONE:
		case BC_ACQUIRE_DONE: {
			struct binder_ptr_cookie pc;
			struct binder_node *node;

			if (binder_get_user(buffer, consumed, &pc, sizeof(pc)) != 0) {
				return EFAULT;
			}
			node = binder_node_find(proc, pc.ptr);
			if (node == NULL) {
				return EINVAL;
			}
			if (cmd == BC_ACQUIRE_DONE) {
				node->pending_strong_ref = false;
			} else {
				node->pending_weak_ref = false;
			}
			break;
		}
		case BC_FREE_BUFFER: {
			binder_uintptr_t data_ptr;

			if (binder_get_user(buffer, consumed, &data_ptr,
			    sizeof(data_ptr)) != 0) {
				return EFAULT;
			}
			ret = binder_write_free_buffer(proc, data_ptr);
			if (ret != 0) {
				return ret;
			}
			break;
		}
		case BC_TRANSACTION:
		case BC_REPLY: {
			struct binder_transaction_data_sg tr_sg;

			bzero(&tr_sg, sizeof(tr_sg));
			if (binder_get_user(buffer, consumed, &tr_sg.transaction_data,
			    sizeof(tr_sg.transaction_data)) != 0) {
				return EFAULT;
			}
			binder_transaction(proc, thread, &tr_sg, cmd == BC_REPLY, false);
			break;
		}
		case BC_TRANSACTION_SG:
		case BC_REPLY_SG: {
			struct binder_transaction_data_sg tr_sg;

			if (binder_get_user(buffer, consumed, &tr_sg, sizeof(tr_sg)) != 0) {
				return EFAULT;
			}
			binder_transaction(proc, thread, &tr_sg, cmd == BC_REPLY_SG, true);
			break;
		}
		case BC_REGISTER_LOOPER:
			if (proc->requested_threads > 0) {
				proc->requested_threads--;
				proc->requested_threads_started++;
			}
			thread->looper |= BINDER_LOOPER_STATE_REGISTERED;
			break;
		case BC_ENTER_LOOPER:
			if (thread->looper & BINDER_LOOPER_STATE_REGISTERED) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
			}
			thread->looper |= BINDER_LOOPER_STATE_ENTERED;
			break;
		case BC_EXIT_LOOPER:
			thread->looper |= BINDER_LOOPER_STATE_EXITED;
			break;
		case BC_REQUEST_DEATH_NOTIFICATION:
		case BC_CLEAR_DEATH_NOTIFICATION: {
			struct binder_handle_cookie hc;

			if (binder_get_user(buffer, consumed, &hc, sizeof(hc)) != 0) {
				return EFAULT;
			}
			ret = (cmd == BC_REQUEST_DEATH_NOTIFICATION) ?
			    binder_death_request(proc, thread, hc.handle, hc.cookie) :
			    binder_death_clear(proc, thread, hc.handle, hc.cookie);
			if (ret != 0) {
				return ret;
			}
			break;
		}
		case BC_DEAD_BINDER_DONE: {
			binder_uintptr_t cookie;

			/* The client acknowledging a death it has finished with. The
			 * record itself belongs to the reference, and is freed when
			 * that goes away, so there is nothing to do but accept it. */
			if (binder_get_user(buffer, consumed, &cookie,
			    sizeof(cookie)) != 0) {
				return EFAULT;
			}
			break;
		}
		case BC_ACQUIRE_RESULT:
		case BC_ATTEMPT_ACQUIRE:
			/* Withdrawn from the protocol before the 64-bit ABI existed. */
			return EINVAL;
		default:
			printf("devfs: binder: unknown command 0x%08x from pid %d\n",
			    cmd, proc->pid);
			return EINVAL;
		}
	}
	return 0;
}

#pragma mark -
#pragma mark The Read Loop

/*
 * Reconcile what the owner of a node has been told with what is now true,
 * and emit whichever of the four reference commands closes the gap. This
 * is why nodes are queued as work rather than commands being emitted from
 * the accounting: a hundred acquire/release pairs collapse into nothing.
 */
static int
binder_read_node_work(struct binder_node *node, user_addr_t buffer,
    uint64_t size, uint64_t *consumed)
{
	struct binder_ptr_cookie pc;
	bool strong = node->internal_strong_refs != 0 || node->local_strong_refs != 0;
	bool weak = strong || node->local_weak_refs != 0 || !LIST_EMPTY(&node->refs);
	int needed = 0;
	int ret;

	pc.ptr = node->ptr;
	pc.cookie = node->cookie;

	/*
	 * Each of the four commands below both emits and records that it has
	 * emitted. Emitting two and running out of room on the third would
	 * leave the node believing the owner had been told things it never
	 * received, so the whole set is costed first and the work is left
	 * queued if it does not fit.
	 */
	if (weak && !node->has_weak_ref) {
		needed++;
	}
	if (strong && !node->has_strong_ref) {
		needed++;
	}
	if (!strong && node->has_strong_ref) {
		needed++;
	}
	if (!weak && node->has_weak_ref) {
		needed++;
	}
	if (needed == 0) {
		return 0;
	}
	if (size - *consumed < (uint64_t)needed * (sizeof(uint32_t) + sizeof(pc))) {
		return ENOMEM;
	}

	if (weak && !node->has_weak_ref) {
		node->has_weak_ref = true;
		node->pending_weak_ref = true;
		ret = binder_put_cmd_data(buffer, size, consumed, BR_INCREFS, &pc, sizeof(pc));
		if (ret != 0) {
			return ret;
		}
	}
	if (strong && !node->has_strong_ref) {
		node->has_strong_ref = true;
		node->pending_strong_ref = true;
		ret = binder_put_cmd_data(buffer, size, consumed, BR_ACQUIRE, &pc, sizeof(pc));
		if (ret != 0) {
			return ret;
		}
	}
	if (!strong && node->has_strong_ref) {
		node->has_strong_ref = false;
		ret = binder_put_cmd_data(buffer, size, consumed, BR_RELEASE, &pc, sizeof(pc));
		if (ret != 0) {
			return ret;
		}
	}
	if (!weak && node->has_weak_ref) {
		node->has_weak_ref = false;
		ret = binder_put_cmd_data(buffer, size, consumed, BR_DECREFS, &pc, sizeof(pc));
		if (ret != 0) {
			return ret;
		}
		binder_node_release(node);
	}
	return 0;
}

/*
 * Hand a transaction to the thread that is about to return to userspace:
 * copy the payload into its arena, then describe it.
 *
 * This is the moment the whole arena design exists for - a copyout to the
 * receiver's address space is legal here and nowhere else, because here
 * the receiver is the current process.
 */
static int
binder_read_transaction(struct binder_proc *proc, struct binder_thread *thread,
    struct binder_transaction *t, user_addr_t buffer, uint64_t size,
    uint64_t *consumed)
{
	struct binder_transaction_data_secctx tr_secctx;
	struct binder_buffer *buf = t->buffer;
	user_addr_t uaddr;
	uint32_t cmd;
	bool is_transaction, want_secctx;
	int ret;
	const char *secctx_str = "u:r:untrusted_app:s0";
	size_t secctx_len = strlen(secctx_str) + 1;

	if (buf == NULL) {
		return EINVAL;
	}
	uaddr = binder_buffer_uaddr(proc, buf);

	if (buf->kdata != NULL &&
	    copyout(buf->kdata, uaddr, buf->kdata_size) != 0) {
		/*
		 * The receiver's arena is gone or was never really there. Its own
		 * fault, but the sender is the one waiting, so the sender is told.
		 */
		printf("devfs: binder: pid %d arena unwritable; failing transaction\n",
		    proc->pid);
		if (t->from != NULL && !t->from->is_dead) {
			t->from->return_error.cmd = BR_FAILED_REPLY;
			t->from->return_error.work.type = BINDER_WORK_RETURN_ERROR;
			binder_enqueue_thread_work(t->from, &t->from->return_error.work);
			binder_wakeup_thread(t->from);
		}
		binder_transaction_buffer_release(proc, buf);
		binder_buffer_free(proc, buf);
		binder_free_transaction(t);
		return 0;
	}

	bzero(&tr_secctx, sizeof(tr_secctx));

	/*
	 * Which of the three forms this delivery takes, which is not a free
	 * choice: it decides how many bytes the client reads next, and whether
	 * it believes it owes a reply.
	 *
	 * A reply is always BR_REPLY. It answers a call the receiver is
	 * already waiting on, carries no security context because nothing is
	 * being granted, and must not be labelled a transaction or the client
	 * will try to answer its own answer. An incoming transaction is
	 * BR_TRANSACTION, or BR_TRANSACTION_SEC_CTX when the target node asked
	 * for a context with FLAT_BINDER_FLAG_TXN_SECURITY_CTX - which is what
	 * servicemanager sets and ordinary services do not.
	 *
	 * Sending everything as BR_TRANSACTION_SEC_CTX broke both halves of
	 * that: replies arrived labelled as calls, and the push below - which
	 * tested for BR_TRANSACTION - stopped happening at all, so no receiver
	 * was ever recorded as owing a reply and every BC_REPLY came back
	 * BR_FAILED_REPLY. Synchronous binder does not work without this.
	 */
	is_transaction = (buf->target_node != NULL);
	want_secctx = is_transaction && (buf->target_node->flags &
	    FLAT_BINDER_FLAG_TXN_SECURITY_CTX) != 0;
	cmd = !is_transaction ? BR_REPLY :
	    (want_secctx ? BR_TRANSACTION_SEC_CTX : BR_TRANSACTION);

	if (buf->target_node != NULL) {
		tr_secctx.transaction_data.target.ptr = buf->target_node->ptr;
		tr_secctx.transaction_data.cookie = buf->target_node->cookie;
	}
	tr_secctx.transaction_data.code = t->code;
	tr_secctx.transaction_data.flags = t->flags;
	tr_secctx.transaction_data.sender_pid = t->sender_pid;
	tr_secctx.transaction_data.sender_euid = t->sender_euid;
	tr_secctx.transaction_data.data_size = buf->data_size;
	tr_secctx.transaction_data.offsets_size = buf->offsets_size;
	tr_secctx.transaction_data.data.ptr.buffer = (binder_uintptr_t)uaddr;
	tr_secctx.transaction_data.data.ptr.offsets = (binder_uintptr_t)(uaddr + BINDER_ALIGN(buf->data_size));

	if (want_secctx) {
		tr_secctx.secctx = (binder_uintptr_t)(buffer + *consumed +
		    sizeof(uint32_t) + sizeof(tr_secctx));

		ret = binder_put_cmd_data(buffer, size, consumed, cmd,
		    &tr_secctx, sizeof(tr_secctx));
		if (ret != 0) {
			return ret;
		}

		if (size - *consumed < secctx_len) {
			return ENOMEM;
		}
		if (copyout(secctx_str, buffer + *consumed, secctx_len) != 0) {
			return EFAULT;
		}
		*consumed += secctx_len;
	} else {
		/* The ordinary forms carry the transaction data alone - the
		 * secctx variant's first member, and nothing after it. */
		ret = binder_put_cmd_data(buffer, size, consumed, cmd,
		    &tr_secctx.transaction_data,
		    sizeof(tr_secctx.transaction_data));
		if (ret != 0) {
			return ret;
		}
	}

	/* From here the client owns the buffer until it says BC_FREE_BUFFER. */
	buf->allow_user_free = true;

	if (is_transaction && t->need_reply) {
		t->to_thread = thread;
		t->to_parent = thread->transaction_stack;
		thread->transaction_stack = t;
	} else {
		binder_free_transaction(t);
	}
	return 0;
}

int
binder_thread_read(struct binder_proc *proc, struct binder_thread *thread,
    user_addr_t buffer, uint64_t size, uint64_t *consumed, bool non_block)
{
	int ret = 0;
	bool wrote_something = false;

	if (*consumed == 0) {
		ret = binder_put_cmd(buffer, size, consumed, BR_NOOP);
		if (ret != 0) {
			return ret;
		}
	}

	for (;;) {
		struct binder_work *w = NULL;
		bool from_proc = false;
		bool found = false;

		if (!TAILQ_EMPTY(&thread->todo)) {
			TAILQ_FOREACH(w, &thread->todo, entry) {
				if (w->type != BINDER_WORK_TRANSACTION ||
				    !thread->frozen) {
					found = true;
					break;
				}
			}
			if (found)
				binder_dequeue_work(w);
		} else if (binder_thread_takes_proc_work(thread) &&
		    !TAILQ_EMPTY(&proc->todo)) {
			TAILQ_FOREACH(w, &proc->todo, entry) {
				if (w->type != BINDER_WORK_TRANSACTION ||
				    !thread->frozen) {
					found = true;
					break;
				}
			}
			if (found) {
				binder_dequeue_work(w);
				from_proc = true;
			}
		}

		if (!found) {
			if (thread->frozen) {
				ret = 0;
				break;
			}
			if (wrote_something) {
				break;      /* give the client what we have */
			}
			if (non_block) {
				ret = EAGAIN;
				break;
			}

			thread->looper |= BINDER_LOOPER_STATE_WAITING;
			if (binder_thread_takes_proc_work(thread)) {
				proc->ready_threads++;
				ret = binder_wait(&proc->todo);
				proc->ready_threads--;
			} else {
				ret = binder_wait(thread);
			}
			thread->looper &= ~BINDER_LOOPER_STATE_WAITING;

			if (ret != 0) {
				/* A signal. The client must go back to userspace to run
				 * its handler; libbinder retries the ioctl. */
				break;
			}
			if (proc->is_dead || thread->is_dead) {
				ret = ENODEV;
				break;
			}
			continue;
		}

		switch (w->type) {
		case BINDER_WORK_TRANSACTION: {
			struct binder_transaction *t =
			    BINDER_CONTAINER_OF(w, struct binder_transaction, work);

			ret = binder_read_transaction(proc, thread, t, buffer, size, consumed);
			break;
		}
		case BINDER_WORK_TRANSACTION_COMPLETE:
			ret = binder_put_cmd(buffer, size, consumed, BR_TRANSACTION_COMPLETE);
			if (ret == 0) {
				binder_free_mem(w, sizeof(*w));
			}
			break;
		case BINDER_WORK_RETURN_ERROR: {
			struct binder_error *e =
			    BINDER_CONTAINER_OF(w, struct binder_error, work);

			ret = binder_put_cmd(buffer, size, consumed, e->cmd);
			if (ret == 0) {
				e->cmd = BR_OK;
			}
			break;
		}
		case BINDER_WORK_NODE: {
			struct binder_node *node =
			    BINDER_CONTAINER_OF(w, struct binder_node, work);

			ret = binder_read_node_work(node, buffer, size, consumed);
			break;
		}
		case BINDER_WORK_DEAD_BINDER:
		case BINDER_WORK_DEAD_BINDER_AND_CLEAR:
		case BINDER_WORK_CLEAR_DEATH_NOTIFICATION: {
			struct binder_ref_death *death =
			    BINDER_CONTAINER_OF(w, struct binder_ref_death, work);
			enum binder_work_type type = w->type;
			uint32_t cmd = (type == BINDER_WORK_CLEAR_DEATH_NOTIFICATION) ?
			    BR_CLEAR_DEATH_NOTIFICATION_DONE : BR_DEAD_BINDER;
			binder_uintptr_t cookie = death->cookie;

			ret = binder_put_cmd_data(buffer, size, consumed, cmd,
			    &cookie, sizeof(cookie));
			if (ret == 0) {
				if (type == BINDER_WORK_DEAD_BINDER_AND_CLEAR) {
					/* The clear was asked for while the death was in
					 * flight; the client is owed both answers. */
					death->work.type = BINDER_WORK_CLEAR_DEATH_NOTIFICATION;
					binder_enqueue_thread_work(thread, &death->work);
				} else if (type == BINDER_WORK_CLEAR_DEATH_NOTIFICATION) {
					binder_free_mem(death, sizeof(*death));
				}
			}
			break;
		}
		default:
			break;
		}

		if (ret == ENOMEM) {
			/*
			 * The client's read buffer is full. Nothing has been
			 * emitted for this item, so put it back at the head of the
			 * queue it came from - dropping it would lose a transaction
			 * outright - and let the next read collect it.
			 */
			binder_queue_insert(from_proc ? &proc->todo : &thread->todo,
			    w, true);
			ret = 0;
			break;
		}
		if (ret != 0) {
			break;
		}
		wrote_something = true;

		/* Ask for another thread if the pool is fully occupied and the
		 * client is allowed more. */
		if (from_proc && proc->requested_threads == 0 &&
		    proc->ready_threads == 0 &&
		    proc->requested_threads_started < proc->max_threads &&
		    (thread->looper & (BINDER_LOOPER_STATE_REGISTERED |
		    BINDER_LOOPER_STATE_ENTERED))) {
			proc->requested_threads++;
			(void)binder_put_cmd(buffer, size, consumed, BR_SPAWN_LOOPER);
		}

		/* Stop while there is still room for the largest single reply. */
		if (size - *consumed < sizeof(uint32_t) +
		    sizeof(struct binder_transaction_data)) {
			break;
		}
	}

	return (ret == ENOMEM) ? 0 : ret;
}

#pragma mark -
#pragma mark BINDER_WRITE_READ

int
binder_ioctl_write_read(struct binder_proc *proc, struct binder_thread *thread,
    struct binder_write_read *bwr)
{
	int ret = 0;

	if (bwr->write_size > 0) {
		ret = binder_thread_write(proc, thread,
		    (user_addr_t)bwr->write_buffer, bwr->write_size,
		    &bwr->write_consumed);
		if (ret != 0) {
			bwr->read_consumed = 0;
			return ret;
		}
	}

	if (bwr->read_size > 0) {
		ret = binder_thread_read(proc, thread,
		    (user_addr_t)bwr->read_buffer, bwr->read_size,
		    &bwr->read_consumed, false);
		if (!TAILQ_EMPTY(&proc->todo)) {
			binder_wakeup_proc(proc);
		}
	}
	return ret;
}
