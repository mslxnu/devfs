/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder-abi-test.c
 *
 * The ABI gate. Every ioctl number and command code in <fs/devfs/binder.h>
 * is written there as a literal, because that is how the mSL/NABI house
 * style writes them and because a reader should be able to compare them
 * against a Linux header by eye. Literals rot. This file recomputes each
 * one from Linux's _IOC encoding applied to the struct the ABI actually
 * names, and fails the build if the two disagree - so a struct that gains
 * a field, or a constant typed one digit out, cannot reach a running
 * kernel. It is compiled, not run: every check below is compile-time.
 *
 * The header this tests was regenerated after the first draft was found to
 * carry a fabricated 24-byte binder_version (Linux's is 4), an inverted
 * direction bit on every BC_/BR_ code, and a constant 0x30 size field.
 * Nothing here would have compiled against that draft, which is the point.
 */

#include <fs/devfs/binder.h>

#include <stdint.h>
#include <stdio.h>

#define CHECK(tag, have, dir, type, nr, size) \
	typedef char abi_check_##tag[((have) == BINDER_IOC(dir, type, nr, size)) ? 1 : -1]

#define W  BINDER_IOC_WRITE
#define R  BINDER_IOC_READ
#define WR (BINDER_IOC_WRITE | BINDER_IOC_READ)
#define N  BINDER_IOC_NONE

/* Ioctls on the device itself. */
CHECK(write_read,      BINDER_WRITE_READ,        WR, 'b',  1, sizeof(struct binder_write_read));
CHECK(idle_timeout,    BINDER_SET_IDLE_TIMEOUT,   W, 'b',  3, sizeof(int64_t));
CHECK(max_threads,     BINDER_SET_MAX_THREADS,    W, 'b',  5, sizeof(uint32_t));
CHECK(idle_priority,   BINDER_SET_IDLE_PRIORITY,  W, 'b',  6, sizeof(int32_t));
CHECK(context_mgr,     BINDER_SET_CONTEXT_MGR,    W, 'b',  7, sizeof(int32_t));
CHECK(thread_exit,     BINDER_THREAD_EXIT,        W, 'b',  8, sizeof(int32_t));
CHECK(version,         BINDER_VERSION,           WR, 'b',  9, sizeof(struct binder_version));
CHECK(node_debug,      BINDER_GET_NODE_DEBUG_INFO,   WR, 'b', 11, sizeof(struct binder_node_debug_info));
CHECK(node_info_ref,   BINDER_GET_NODE_INFO_FOR_REF, WR, 'b', 12, sizeof(struct binder_node_info_for_ref));
CHECK(context_mgr_ext, BINDER_SET_CONTEXT_MGR_EXT,    W, 'b', 13, sizeof(struct flat_binder_object));
CHECK(freeze,          BINDER_FREEZE,                 W, 'b', 14, sizeof(struct binder_freeze_info));
CHECK(frozen_info,     BINDER_GET_FROZEN_INFO,       WR, 'b', 15, sizeof(struct binder_frozen_status_info));
CHECK(oneway_spam,     BINDER_ENABLE_ONEWAY_SPAM_DETECTION, W, 'b', 16, sizeof(uint32_t));
CHECK(extended_error,  BINDER_GET_EXTENDED_ERROR,    WR, 'b', 17, sizeof(struct binder_extended_error));
CHECK(ctl_add,         BINDER_CTL_ADD,               WR, 'b',  1, sizeof(struct binderfs_device));

/* mSL extensions. Both directions, since they are ours and never travel
 * as Linux numbers - see BINDER_CMD_HOST in the header. */
CHECK(msl_arena,   BINDER_MSL_SET_ARENA,   WR, 'b', 0xE0, sizeof(struct binder_msl_arena));
CHECK(msl_version, BINDER_MSL_ABI_VERSION, WR, 'b', 0xE1, sizeof(uint32_t));

/* Driver returns. */
CHECK(br_error,       BR_ERROR,               R, 'r',  0, sizeof(int32_t));
CHECK(br_ok,          BR_OK,                  N, 'r',  1, 0);
CHECK(br_txn_secctx,  BR_TRANSACTION_SEC_CTX, R, 'r',  2, sizeof(struct binder_transaction_data_secctx));
CHECK(br_txn,         BR_TRANSACTION,         R, 'r',  2, sizeof(struct binder_transaction_data));
CHECK(br_reply,       BR_REPLY,               R, 'r',  3, sizeof(struct binder_transaction_data));
CHECK(br_acq_result,  BR_ACQUIRE_RESULT,      R, 'r',  4, sizeof(int32_t));
CHECK(br_dead_reply,  BR_DEAD_REPLY,          N, 'r',  5, 0);
CHECK(br_txn_done,    BR_TRANSACTION_COMPLETE,N, 'r',  6, 0);
CHECK(br_increfs,     BR_INCREFS,             R, 'r',  7, sizeof(struct binder_ptr_cookie));
CHECK(br_acquire,     BR_ACQUIRE,             R, 'r',  8, sizeof(struct binder_ptr_cookie));
CHECK(br_release,     BR_RELEASE,             R, 'r',  9, sizeof(struct binder_ptr_cookie));
CHECK(br_decrefs,     BR_DECREFS,             R, 'r', 10, sizeof(struct binder_ptr_cookie));
CHECK(br_attempt_acq, BR_ATTEMPT_ACQUIRE,     R, 'r', 11, sizeof(struct binder_pri_ptr_cookie));
CHECK(br_noop,        BR_NOOP,                N, 'r', 12, 0);
CHECK(br_spawn,       BR_SPAWN_LOOPER,        N, 'r', 13, 0);
CHECK(br_finished,    BR_FINISHED,            N, 'r', 14, 0);
CHECK(br_dead_binder, BR_DEAD_BINDER,         R, 'r', 15, sizeof(binder_uintptr_t));
CHECK(br_clear_death, BR_CLEAR_DEATH_NOTIFICATION_DONE, R, 'r', 16, sizeof(binder_uintptr_t));
CHECK(br_failed,      BR_FAILED_REPLY,        N, 'r', 17, 0);
CHECK(br_frozen,      BR_FROZEN_REPLY,        N, 'r', 18, 0);
CHECK(br_spam,        BR_ONEWAY_SPAM_SUSPECT, N, 'r', 19, 0);
CHECK(br_pending,     BR_TRANSACTION_PENDING_FROZEN, N, 'r', 20, 0);

/* Driver commands. */
CHECK(bc_txn,         BC_TRANSACTION,      W, 'c',  0, sizeof(struct binder_transaction_data));
CHECK(bc_reply,       BC_REPLY,            W, 'c',  1, sizeof(struct binder_transaction_data));
CHECK(bc_acq_result,  BC_ACQUIRE_RESULT,   W, 'c',  2, sizeof(int32_t));
CHECK(bc_free_buffer, BC_FREE_BUFFER,      W, 'c',  3, sizeof(binder_uintptr_t));
CHECK(bc_increfs,     BC_INCREFS,          W, 'c',  4, sizeof(uint32_t));
CHECK(bc_acquire,     BC_ACQUIRE,          W, 'c',  5, sizeof(uint32_t));
CHECK(bc_release,     BC_RELEASE,          W, 'c',  6, sizeof(uint32_t));
CHECK(bc_decrefs,     BC_DECREFS,          W, 'c',  7, sizeof(uint32_t));
CHECK(bc_increfs_done,BC_INCREFS_DONE,     W, 'c',  8, sizeof(struct binder_ptr_cookie));
CHECK(bc_acquire_done,BC_ACQUIRE_DONE,     W, 'c',  9, sizeof(struct binder_ptr_cookie));
CHECK(bc_attempt_acq, BC_ATTEMPT_ACQUIRE,  W, 'c', 10, sizeof(struct binder_pri_desc));
CHECK(bc_reg_looper,  BC_REGISTER_LOOPER,  N, 'c', 11, 0);
CHECK(bc_enter_looper,BC_ENTER_LOOPER,     N, 'c', 12, 0);
CHECK(bc_exit_looper, BC_EXIT_LOOPER,      N, 'c', 13, 0);
CHECK(bc_req_death,   BC_REQUEST_DEATH_NOTIFICATION, W, 'c', 14, sizeof(struct binder_handle_cookie));
CHECK(bc_clear_death, BC_CLEAR_DEATH_NOTIFICATION,   W, 'c', 15, sizeof(struct binder_handle_cookie));
CHECK(bc_death_done,  BC_DEAD_BINDER_DONE, W, 'c', 16, sizeof(binder_uintptr_t));
CHECK(bc_txn_sg,      BC_TRANSACTION_SG,   W, 'c', 17, sizeof(struct binder_transaction_data_sg));
CHECK(bc_reply_sg,    BC_REPLY_SG,         W, 'c', 18, sizeof(struct binder_transaction_data_sg));

/*
 * The direction-bit rule the driver depends on: forcing both bits on must
 * change nothing but the direction, and masking them off must leave the
 * two spellings indistinguishable.
 */
typedef char abi_check_host_sets_both[
    (BINDER_CMD_HOST(BINDER_SET_CONTEXT_MGR) == 0xC0046207u) ? 1 : -1];
typedef char abi_check_key_ignores_dir[
    (BINDER_CMD_KEY(BINDER_CMD_HOST(BINDER_SET_CONTEXT_MGR)) ==
     BINDER_CMD_KEY(BINDER_SET_CONTEXT_MGR)) ? 1 : -1];
typedef char abi_check_wr_already_both[
    (BINDER_CMD_HOST(BINDER_WRITE_READ) == BINDER_WRITE_READ) ? 1 : -1];

int
main(void)
{
	printf("binder ABI: protocol %d, mSL extensions rev %d\n",
	    BINDER_CURRENT_PROTOCOL_VERSION, BINDER_MSL_ABI_CURRENT);
	printf("  every ioctl number and command code matches its _IOC "
	    "derivation (checked at compile time)\n");
	return 0;
}
