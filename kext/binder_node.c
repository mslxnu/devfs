/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder_node.c
 *
 * Objects and the handles that name them, and the reference counting that
 * decides when an object stops existing.
 *
 * The shape is Linux's, because the protocol is: a node is something a
 * process implements, a ref is another process's handle to it, and the
 * driver keeps two counts of each - the refs other processes hold
 * ("internal"), and the ones the owner itself holds ("local"). Neither
 * count is the owner's business until it crosses zero, and then the owner
 * is told with BR_ACQUIRE / BR_RELEASE (strong) or BR_INCREFS / BR_DECREFS
 * (weak) and answers BC_ACQUIRE_DONE / BC_INCREFS_DONE.
 *
 * The subtle part, and the reason this is worth reading before changing:
 * the driver never sends those commands from inside the accounting. It
 * queues the node itself as a work item and lets binder_thread_read()
 * compare what the owner has been told against what is now true. That is
 * what makes a burst of acquire/release traffic collapse into the two
 * commands that actually changed something, and it is why has_strong_ref
 * and pending_strong_ref are separate flags.
 *
 * tmp_refs exist for one reason: this driver holds a single lock and drops
 * it around copyin/copyout, so a node that is merely "in play" during a
 * transaction - not yet referenced by anybody - has to survive that gap.
 */

#include <fs/devfs/binder_internal.h>

#include <libkern/libkern.h>
#include <libkext.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/proc.h>

#pragma mark -
#pragma mark Nodes

struct binder_node *
binder_node_find(struct binder_proc *proc, binder_uintptr_t ptr)
{
	struct binder_node *node;

	LIST_FOREACH(node, &proc->nodes, proc_entry) {
		if (node->ptr == ptr) {
			return node;
		}
	}
	return NULL;
}

struct binder_node *
binder_node_new(struct binder_proc *proc, binder_uintptr_t ptr,
    binder_uintptr_t cookie, uint32_t flags)
{
	struct binder_node *node;

	node = binder_node_find(proc, ptr);
	if (node != NULL) {
		/*
		 * Two objects at one address is the owner contradicting itself.
		 * Linux takes the cookie of the first and carries on, because the
		 * alternative is failing a transaction over bookkeeping the sender
		 * cannot see; do the same.
		 */
		return node;
	}

	node = binder_alloc_mem(sizeof(*node));
	if (node == NULL) {
		return NULL;
	}
	node->proc = proc;
	node->ptr = ptr;
	node->cookie = cookie;
	node->flags = flags;
	node->accept_fds = (flags & FLAT_BINDER_FLAG_ACCEPTS_FDS) != 0;
	node->work.type = BINDER_WORK_NODE;
	node->debug_id = binder_next_debug_id();
	LIST_INIT(&node->refs);
	TAILQ_INIT(&node->async_todo);

	LIST_INSERT_HEAD(&proc->nodes, node, proc_entry);
	binder_stat_nodes++;
	return node;
}

void
binder_node_inc_tmpref(struct binder_node *node)
{
	node->tmp_refs++;
}

/*
 * Whether a node still has a reason to exist. Weak liveness includes the
 * existence of any ref at all: a handle somebody holds is itself a claim,
 * even one whose counts have gone to zero on the way to being deleted.
 */
static bool
binder_node_has_strong(const struct binder_node *node)
{
	return node->internal_strong_refs != 0 || node->local_strong_refs != 0;
}

static bool
binder_node_has_weak(const struct binder_node *node)
{
	return binder_node_has_strong(node) || node->local_weak_refs != 0 ||
	    !LIST_EMPTY(&node->refs);
}

/*
 * Free a node once nothing points at it: no refs, no counts, no temporary
 * hold, and the owner either gone or already told to let go.
 */
static void
binder_node_maybe_free(struct binder_node *node)
{
	struct binder_work *w, *tmp;

	if (node->tmp_refs != 0 || binder_node_has_weak(node)) {
		return;
	}
	if (node->has_strong_ref || node->has_weak_ref) {
		return;   /* the owner has not acknowledged the release yet */
	}
	if (node->work.queued) {
		return;   /* still queued as news for somebody */
	}

	TAILQ_FOREACH_SAFE(w, &node->async_todo, entry, tmp) {
		TAILQ_REMOVE(&node->async_todo, w, entry);
		w->queued = false;
	}

	if (node->proc != NULL) {
		LIST_REMOVE(node, proc_entry);
	}
	if (node->proc != NULL && node->proc->context != NULL &&
	    node->proc->context->mgr_node == node) {
		node->proc->context->mgr_node = NULL;
	}
	binder_free_mem(node, sizeof(*node));
	binder_stat_nodes--;
}

void
binder_node_dec_tmpref(struct binder_node *node)
{
	node->tmp_refs--;
	binder_node_maybe_free(node);
}

/*
 * Queue the node to its owner so binder_thread_read() can work out which,
 * if any, of BR_INCREFS/ACQUIRE/RELEASE/DECREFS the owner now needs. A
 * node already queued needs nothing: the read will see the latest counts,
 * not the ones in force when it was queued.
 */
static void
binder_node_notify_owner(struct binder_node *node, struct binder_thread *thread)
{
	if (node->proc == NULL || node->proc->is_dead) {
		return;
	}
	if (node->work.queued) {
		return;
	}
	if (thread != NULL && thread->proc == node->proc) {
		binder_enqueue_thread_work(thread, &node->work);
	} else {
		binder_enqueue_work(node->proc, &node->work);
	}
}

/*
 * The one entry point for changing a node's counts. `internal` means the
 * change came from a handle in another process rather than from the
 * owner's own BC_INCREFS/BC_ACQUIRE.
 */
int
binder_node_update_refs(struct binder_node *node, bool strong, bool increment,
    bool internal, struct binder_thread *thread)
{
	bool was_strong = binder_node_has_strong(node);
	bool was_weak = binder_node_has_weak(node);

	if (strong) {
		if (increment) {
			if (internal) {
				node->internal_strong_refs++;
			} else {
				node->local_strong_refs++;
			}
		} else {
			if (internal) {
				if (node->internal_strong_refs == 0) {
					return EINVAL;
				}
				node->internal_strong_refs--;
			} else {
				if (node->local_strong_refs == 0) {
					return EINVAL;
				}
				node->local_strong_refs--;
			}
		}
	} else {
		if (increment) {
			if (!internal) {
				node->local_weak_refs++;
			}
		} else {
			if (!internal) {
				if (node->local_weak_refs == 0) {
					return EINVAL;
				}
				node->local_weak_refs--;
			}
		}
	}

	if (binder_node_has_strong(node) != was_strong ||
	    binder_node_has_weak(node) != was_weak) {
		binder_node_notify_owner(node, thread);
	}
	binder_node_maybe_free(node);
	return 0;
}

/*
 * The owner is gone. Every handle to this node becomes dead: holders that
 * asked for a death notification get one, and the node itself is detached
 * from the process that is on its way out.
 */
void
binder_node_died(struct binder_node *node)
{
	struct binder_ref *ref;

	node->is_dead = true;
	node->proc = NULL;

	LIST_FOREACH(ref, &node->refs, node_entry) {
		if (ref->death == NULL) {
			continue;
		}
		if (ref->death->work.queued) {
			continue;
		}
		ref->death->work.type = BINDER_WORK_DEAD_BINDER;
		binder_enqueue_work(ref->proc, &ref->death->work);
		binder_wakeup_proc(ref->proc);
	}
}

void
binder_node_release(struct binder_node *node)
{
	binder_node_maybe_free(node);
}

#pragma mark -
#pragma mark References

struct binder_ref *
binder_ref_find_by_desc(struct binder_proc *proc, uint32_t desc)
{
	struct binder_ref *ref;

	LIST_FOREACH(ref, &proc->refs, proc_entry) {
		if (ref->desc == desc) {
			return ref;
		}
	}
	return NULL;
}

static struct binder_ref *
binder_ref_find_for_node(struct binder_proc *proc, struct binder_node *node)
{
	struct binder_ref *ref;

	LIST_FOREACH(ref, &proc->refs, proc_entry) {
		if (ref->node == node) {
			return ref;
		}
	}
	return NULL;
}

/*
 * Handle 0 is the context manager and nothing else; every other handle is
 * the lowest number not currently in use, which is what makes a handle
 * space compact enough for a client to keep in an array.
 */
static uint32_t
binder_ref_next_desc(struct binder_proc *proc, struct binder_node *node)
{
	struct binder_context *ctx = proc->context;
	uint32_t desc;

	if (ctx != NULL && ctx->mgr_node == node) {
		return 0;
	}
	for (desc = 1; desc != 0; desc++) {
		if (binder_ref_find_by_desc(proc, desc) == NULL) {
			return desc;
		}
	}
	return 0;
}

struct binder_ref *
binder_ref_get_for_node(struct binder_proc *proc, struct binder_node *node)
{
	struct binder_ref *ref;

	ref = binder_ref_find_for_node(proc, node);
	if (ref != NULL) {
		return ref;
	}

	ref = binder_alloc_mem(sizeof(*ref));
	if (ref == NULL) {
		return NULL;
	}
	ref->proc = proc;
	ref->node = node;
	ref->desc = binder_ref_next_desc(proc, node);
	ref->debug_id = binder_next_debug_id();

	LIST_INSERT_HEAD(&proc->refs, ref, proc_entry);
	LIST_INSERT_HEAD(&node->refs, ref, node_entry);
	binder_stat_refs++;
	return ref;
}

/*
 * A ref's counts are the process's, and the node only hears about the
 * crossings: the first strong ref a process takes is what makes the owner
 * acquire, and the last one released is what lets it go.
 */
int
binder_ref_update(struct binder_ref *ref, bool strong, bool increment,
    struct binder_thread *thread)
{
	int ret = 0;

	if (strong) {
		if (increment) {
			if (ref->strong == 0) {
				ret = binder_node_update_refs(ref->node, true, true, true, thread);
				if (ret != 0) {
					return ret;
				}
			}
			ref->strong++;
		} else {
			if (ref->strong == 0) {
				return EINVAL;
			}
			ref->strong--;
			if (ref->strong == 0) {
				ret = binder_node_update_refs(ref->node, true, false, true, thread);
			}
		}
	} else {
		if (increment) {
			ref->weak++;
		} else {
			if (ref->weak == 0) {
				return EINVAL;
			}
			ref->weak--;
		}
	}

	if (ref->strong == 0 && ref->weak == 0) {
		binder_ref_delete(ref);
	}
	return ret;
}

void
binder_ref_delete(struct binder_ref *ref)
{
	struct binder_node *node = ref->node;

	if (ref->death != NULL) {
		binder_dequeue_work(&ref->death->work);
		binder_free_mem(ref->death, sizeof(*ref->death));
		ref->death = NULL;
	}

	LIST_REMOVE(ref, proc_entry);
	LIST_REMOVE(ref, node_entry);
	binder_free_mem(ref, sizeof(*ref));
	binder_stat_refs--;

	if (node != NULL) {
		/* The node may have been kept alive only by this handle. */
		binder_node_update_refs(node, false, false, true, NULL);
		binder_node_maybe_free(node);
	}
}

#pragma mark -
#pragma mark The Context Manager

/*
 * Becoming servicemanager. Linux allows exactly one per context and, since
 * 5.x, records the caller's uid so a second process cannot take the role
 * after the first exits unless it is the same user. The uid check is worth
 * keeping even here, where there is no SELinux behind it, because it is
 * what stops an ordinary process from impersonating the manager of a
 * context an Android system started.
 */
int
binder_context_mgr_set(struct binder_proc *proc, binder_uintptr_t ptr,
    binder_uintptr_t cookie, uint32_t flags)
{
	struct binder_context *ctx = proc->context;
	struct binder_node *node;
	kauth_cred_t cred;
	uid_t uid;

	if (ctx == NULL) {
		return EINVAL;
	}
	if (ctx->mgr_node != NULL) {
		return EBUSY;
	}

	/*
	 * kauth_cred_get() borrows the calling thread's credential, which is
	 * valid for the length of this syscall and needs no reference - and it
	 * avoids current_proc(), whose declaration moves between the public and
	 * the kernel-private proc.h depending on how the tree is configured.
	 */
	cred = kauth_cred_get();
	uid = kauth_cred_getuid(cred);

	if (ctx->mgr_uid_valid) {
		if (ctx->mgr_uid != uid) {
			printf("devfs: binder: %s context manager is uid %u, "
			    "refusing uid %u\n", ctx->name, ctx->mgr_uid, uid);
			return EPERM;
		}
	} else {
		ctx->mgr_uid = uid;
		ctx->mgr_uid_valid = true;
	}

	/*
	 * The manager's node is created before it is named as the manager, so
	 * that binder_ref_next_desc() can recognise it and hand out handle 0.
	 */
	node = binder_node_new(proc, ptr, cookie, flags);
	if (node == NULL) {
		return ENOMEM;
	}
	ctx->mgr_node = node;

	/*
	 * The manager holds one strong and one weak reference to itself for as
	 * long as it is the manager, so the node cannot be freed underneath a
	 * client that is about to ask for handle 0.
	 */
	node->local_strong_refs++;
	node->local_weak_refs++;
	node->has_strong_ref = true;
	node->has_weak_ref = true;
	return 0;
}

#pragma mark -
#pragma mark Death Notifications

/*
 * "Tell me when the object behind this handle goes away." The cookie is
 * the client's, echoed back in BR_DEAD_BINDER so it can find its own
 * bookkeeping without a search.
 */
int
binder_death_request(struct binder_proc *proc, struct binder_thread *thread,
    uint32_t handle, binder_uintptr_t cookie)
{
	struct binder_ref *ref;
	struct binder_ref_death *death;

	ref = binder_ref_find_by_desc(proc, handle);
	if (ref == NULL) {
		return EINVAL;
	}
	if (ref->death != NULL) {
		return EINVAL;   /* one notification per handle, as on Linux */
	}

	death = binder_alloc_mem(sizeof(*death));
	if (death == NULL) {
		return ENOMEM;
	}
	death->cookie = cookie;
	death->work.type = BINDER_WORK_DEAD_BINDER;
	ref->death = death;

	/*
	 * Registering against something already dead is not an error - the
	 * answer is simply immediate.
	 */
	if (ref->node != NULL && ref->node->is_dead) {
		binder_enqueue_thread_work(thread, &death->work);
	}
	return 0;
}

int
binder_death_clear(struct binder_proc *proc, struct binder_thread *thread,
    uint32_t handle, binder_uintptr_t cookie)
{
	struct binder_ref *ref;
	struct binder_ref_death *death;

	ref = binder_ref_find_by_desc(proc, handle);
	if (ref == NULL || ref->death == NULL) {
		return EINVAL;
	}
	death = ref->death;
	if (death->cookie != cookie) {
		return EINVAL;
	}

	if (!death->work.queued) {
		death->work.type = BINDER_WORK_CLEAR_DEATH_NOTIFICATION;
		binder_enqueue_thread_work(thread, &death->work);
	} else {
		/*
		 * A death is already on its way to the client. It has to be told
		 * that the clear happened too, and only after it has acknowledged
		 * the death - so the work item changes meaning in place rather
		 * than being queued twice.
		 */
		death->work.type = BINDER_WORK_DEAD_BINDER_AND_CLEAR;
	}
	ref->death = NULL;

	/* The client owns the notification's memory until it reads it. */
	return 0;
}
