/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder.h
 *
 * Shared wire protocol for the Linux binder ABI on /dev/binder et al.
 * This header is included by both the kext and userspace tools/NABI.
 *
 * The binder wire structs and ioctl numbers here are the Linux binder ABI
 * on the wire (protocol version 8, scatter-gather + flat_binder_object
 * security ctx). Converted from Linux include/uapi/linux/android/binder.h
 * into the LINUX_/l_* house style.
 *
 * The mSL-NABI include/linux/binder.h (M1) is the cross-repo contract:
 * both headers must be byte-identical on the wire structs and ioctl values.
 */

#ifndef _FS_DEVFS_BINDER_H_
#define _FS_DEVFS_BINDER_H_

#include <stdint.h>

#pragma mark -
#pragma mark Binder Protocol Version

/*
 * Binder protocol version (the value BINDER_VERSION ioctl returns).
 * Protocol version 8 = scatter-gather + flat_binder_object security ctx.
 */
#define LINUX_BINDER_CURRENT_PROTOCOL_VERSION 8

/*
 * B_PACK_CHARS from Linux binder.h - packs 4 chars into a u32.
 */
#define LINUX_B_PACK_CHARS(c1, c2, c3, c4) \
	((((uint32_t)(c1) << 24) | ((uint32_t)(c2) << 16) | ((uint32_t)(c3) << 8) | (uint32_t)(c4)))
#define LINUX_B_TYPE_LARGE 0x85

/*
 * Binder object types.
 */
#define LINUX_BINDER_TYPE_BINDER      LINUX_B_PACK_CHARS('s', 'b', '*', LINUX_B_TYPE_LARGE)
#define LINUX_BINDER_TYPE_WEAK_BINDER LINUX_B_PACK_CHARS('w', 'b', '*', LINUX_B_TYPE_LARGE)
#define LINUX_BINDER_TYPE_HANDLE      LINUX_B_PACK_CHARS('s', 'h', '*', LINUX_B_TYPE_LARGE)
#define LINUX_BINDER_TYPE_WEAK_HANDLE LINUX_B_PACK_CHARS('w', 'h', '*', LINUX_B_TYPE_LARGE)
#define LINUX_BINDER_TYPE_FD          LINUX_B_PACK_CHARS('f', 'd', '*', LINUX_B_TYPE_LARGE)

/*
 * flat_binder_object flags.
 */
#define LINUX_FLAT_BINDER_FLAG_PRIORITY_MASK 0xff
#define LINUX_FLAT_BINDER_FLAG_ACCEPTS_FDS   0x100

/*
 * Architecture-dependent pointer/size types.
 * The kext is 64-bit; NABI guest may be 32-bit. The wire uses 64-bit.
 */
typedef uint64_t l_binder_size_t;
typedef uint64_t l_binder_uintptr_t;

#pragma mark -
#pragma mark Binder Wire Structs

/*
 * This is the flattened representation of a Binder object for transfer
 * between processes.  The 'offsets' supplied as part of a binder transaction
 * contains offsets into the data where these structures occur.  The Binder
 * driver takes care of re-writing the structure type and data as it moves
 * between processes.
 */
struct l_flat_binder_object {
	/* 8 bytes for large_flat_header. */
	uint32_t	type;
	uint32_t	flags;

	/* 8 bytes of data. */
	union {
		l_binder_uintptr_t	binder;	/* local object */
		uint32_t		handle;	/* remote object */
	};

	/* extra data associated with local object */
	l_binder_uintptr_t	cookie;
};

/*
 * binder_write_read - the main read/write ioctl structure.
 */
struct l_binder_write_read {
	l_binder_size_t		write_size;		/* bytes to write */
	l_binder_size_t		write_consumed;		/* bytes consumed by driver */
	l_binder_uintptr_t	write_buffer;
	l_binder_size_t		read_size;		/* bytes to read */
	l_binder_size_t		read_consumed;		/* bytes consumed by driver */
	l_binder_uintptr_t	read_buffer;
};

/*
 * binder_version - for BINDER_VERSION ioctl.
 */
struct l_binder_version {
	int32_t protocol_version;
	int32_t reserved[5];  /* padding to 24 bytes per ioctl encoding */
};

/*
 * Transaction flags.
 */
#define LINUX_TF_ONE_WAY       0x01
#define LINUX_TF_ROOT_OBJECT   0x04
#define LINUX_TF_STATUS_CODE   0x08
#define LINUX_TF_ACCEPT_FDS    0x10

/*
 * binder_transaction_data - the transaction payload.
 */
struct l_binder_transaction_data {
	union {
		uint32_t			handle;		/* target descriptor */
		l_binder_uintptr_t	ptr;		/* target ptr for reply */
	} target;
	l_binder_uintptr_t	cookie;
	uint32_t		code;
	uint32_t		flags;
	int32_t			sender_pid;
	uint32_t		sender_euid;
	l_binder_size_t	data_size;
	l_binder_size_t	offsets_size;

	union {
		struct {
			l_binder_uintptr_t	buffer;
			l_binder_uintptr_t	offsets;
		} ptr;
		uint8_t	buf[8];
	} data;
};

/*
 * Supporting structs for refcount/death commands.
 */
struct l_binder_ptr_cookie {
	l_binder_uintptr_t ptr;
	l_binder_uintptr_t cookie;
};

struct l_binder_handle_cookie {
	uint32_t handle;
	l_binder_uintptr_t cookie;
} __attribute__((packed));

struct l_binder_pri_desc {
	int32_t priority;
	uint32_t desc;
};

struct l_binder_pri_ptr_cookie {
	int32_t priority;
	l_binder_uintptr_t ptr;
	l_binder_uintptr_t cookie;
};

#pragma mark -
#pragma mark Driver Return Protocol (BR_*)

/*
 * Return codes from driver to userspace.
 * These appear in the read_buffer of binder_write_read.
 */
#define LINUX_BR_ERROR                            0x00007200  /* _IOR('r', 0, int32_t) */
#define LINUX_BR_OK                               0x00007201  /* _IO('r', 1) */
#define LINUX_BR_TRANSACTION                      0x40307202  /* _IOR('r', 2, binder_transaction_data) */
#define LINUX_BR_REPLY                            0x40307203  /* _IOR('r', 3, binder_transaction_data) */
#define LINUX_BR_ACQUIRE_RESULT                   0x00007204  /* _IOR('r', 4, int32_t) */
#define LINUX_BR_DEAD_REPLY                       0x00007205  /* _IO('r', 5) */
#define LINUX_BR_TRANSACTION_COMPLETE             0x00007206  /* _IO('r', 6) */
#define LINUX_BR_INCREFS                          0x40107207  /* _IOR('r', 7, binder_ptr_cookie) */
#define LINUX_BR_ACQUIRE                          0x40107208  /* _IOR('r', 8, binder_ptr_cookie) */
#define LINUX_BR_RELEASE                          0x40107209  /* _IOR('r', 9, binder_ptr_cookie) */
#define LINUX_BR_DECREFS                          0x4010720A  /* _IOR('r', 10, binder_ptr_cookie) */
#define LINUX_BR_ATTEMPT_ACQUIRE                  0x4014720B  /* _IOR('r', 11, binder_pri_ptr_cookie) */
#define LINUX_BR_NOOP                             0x0000720C  /* _IO('r', 12) */
#define LINUX_BR_SPAWN_LOOPER                     0x0000720D  /* _IO('r', 13) */
#define LINUX_BR_FINISHED                         0x0000720E  /* _IO('r', 14) */
#define LINUX_BR_DEAD_BINDER                      0x4008720F  /* _IOR('r', 15, binder_uintptr_t) */
#define LINUX_BR_CLEAR_DEATH_NOTIFICATION_DONE    0x40087210  /* _IOR('r', 16, binder_uintptr_t) */
#define LINUX_BR_FAILED_REPLY                     0x00007211  /* _IO('r', 17) */

/*
 * Convenience enum for switch statements.
 */
enum l_binder_driver_return_protocol {
	L_BR_ERROR = LINUX_BR_ERROR,
	L_BR_OK = LINUX_BR_OK,
	L_BR_TRANSACTION = LINUX_BR_TRANSACTION,
	L_BR_REPLY = LINUX_BR_REPLY,
	L_BR_ACQUIRE_RESULT = LINUX_BR_ACQUIRE_RESULT,
	L_BR_DEAD_REPLY = LINUX_BR_DEAD_REPLY,
	L_BR_TRANSACTION_COMPLETE = LINUX_BR_TRANSACTION_COMPLETE,
	L_BR_INCREFS = LINUX_BR_INCREFS,
	L_BR_ACQUIRE = LINUX_BR_ACQUIRE,
	L_BR_RELEASE = LINUX_BR_RELEASE,
	L_BR_DECREFS = LINUX_BR_DECREFS,
	L_BR_ATTEMPT_ACQUIRE = LINUX_BR_ATTEMPT_ACQUIRE,
	L_BR_NOOP = LINUX_BR_NOOP,
	L_BR_SPAWN_LOOPER = LINUX_BR_SPAWN_LOOPER,
	L_BR_FINISHED = LINUX_BR_FINISHED,
	L_BR_DEAD_BINDER = LINUX_BR_DEAD_BINDER,
	L_BR_CLEAR_DEATH_NOTIFICATION_DONE = LINUX_BR_CLEAR_DEATH_NOTIFICATION_DONE,
	L_BR_FAILED_REPLY = LINUX_BR_FAILED_REPLY,
};

#pragma mark -
#pragma mark Driver Command Protocol (BC_*)

/*
 * Commands from userspace to driver.
 * These appear in the write_buffer of binder_write_read.
 */
#define LINUX_BC_TRANSACTION                      0xC0306300  /* _IOW('c', 0, binder_transaction_data) */
#define LINUX_BC_REPLY                            0xC0306301  /* _IOW('c', 1, binder_transaction_data) */
#define LINUX_BC_ACQUIRE_RESULT                   0x40046302  /* _IOW('c', 2, int32_t) */
#define LINUX_BC_FREE_BUFFER                      0x40086303  /* _IOW('c', 3, binder_uintptr_t) */
#define LINUX_BC_INCREFS                          0x40046304  /* _IOW('c', 4, uint32_t) */
#define LINUX_BC_ACQUIRE                          0x40046305  /* _IOW('c', 5, uint32_t) */
#define LINUX_BC_RELEASE                          0x40046306  /* _IOW('c', 6, uint32_t) */
#define LINUX_BC_DECREFS                          0x40046307  /* _IOW('c', 7, uint32_t) */
#define LINUX_BC_INCREFS_DONE                     0x40106308  /* _IOW('c', 8, binder_ptr_cookie) */
#define LINUX_BC_ACQUIRE_DONE                     0x40106309  /* _IOW('c', 9, binder_ptr_cookie) */
#define LINUX_BC_ATTEMPT_ACQUIRE                  0x4008630A  /* _IOW('c', 10, binder_pri_desc) */
#define LINUX_BC_REGISTER_LOOPER                  0x0000630B  /* _IO('c', 11) */
#define LINUX_BC_ENTER_LOOPER                     0x0000630C  /* _IO('c', 12) */
#define LINUX_BC_EXIT_LOOPER                      0x0000630D  /* _IO('c', 13) */
#define LINUX_BC_REQUEST_DEATH_NOTIFICATION       0x400C630E  /* _IOW('c', 14, binder_handle_cookie) */
#define LINUX_BC_CLEAR_DEATH_NOTIFICATION         0x400C630F  /* _IOW('c', 15, binder_handle_cookie) */
#define LINUX_BC_DEAD_BINDER_DONE                 0x40086310  /* _IOW('c', 16, binder_uintptr_t) */

/*
 * Convenience enum for switch statements.
 */
enum l_binder_driver_command_protocol {
	L_BC_TRANSACTION = LINUX_BC_TRANSACTION,
	L_BC_REPLY = LINUX_BC_REPLY,
	L_BC_ACQUIRE_RESULT = LINUX_BC_ACQUIRE_RESULT,
	L_BC_FREE_BUFFER = LINUX_BC_FREE_BUFFER,
	L_BC_INCREFS = LINUX_BC_INCREFS,
	L_BC_ACQUIRE = LINUX_BC_ACQUIRE,
	L_BC_RELEASE = LINUX_BC_RELEASE,
	L_BC_DECREFS = LINUX_BC_DECREFS,
	L_BC_INCREFS_DONE = LINUX_BC_INCREFS_DONE,
	L_BC_ACQUIRE_DONE = LINUX_BC_ACQUIRE_DONE,
	L_BC_ATTEMPT_ACQUIRE = LINUX_BC_ATTEMPT_ACQUIRE,
	L_BC_REGISTER_LOOPER = LINUX_BC_REGISTER_LOOPER,
	L_BC_ENTER_LOOPER = LINUX_BC_ENTER_LOOPER,
	L_BC_EXIT_LOOPER = LINUX_BC_EXIT_LOOPER,
	L_BC_REQUEST_DEATH_NOTIFICATION = LINUX_BC_REQUEST_DEATH_NOTIFICATION,
	L_BC_CLEAR_DEATH_NOTIFICATION = LINUX_BC_CLEAR_DEATH_NOTIFICATION,
	L_BC_DEAD_BINDER_DONE = LINUX_BC_DEAD_BINDER_DONE,
};

#pragma mark -
#pragma mark Binder Ioctl Numbers

/*
 * Ioctl numbers — Linux ABI verbatim.
 * _IOC(dir, type, nr, size) where type='b'=0x62.
 * Dir: _IOC_NONE=0, _IOC_WRITE=1, _IOC_READ=2
 */
#define LINUX_BINDER_WRITE_READ        0xC0306201  /* _IOWR('b', 1, binder_write_read)  size 48 */
#define LINUX_BINDER_SET_MAX_THREADS   0x40046205  /* _IOW ('b', 5, uint32_t)           size 4  */
#define LINUX_BINDER_SET_CONTEXT_MGR   0x40046207  /* _IOW ('b', 7, int32_t)            size 4  */
#define LINUX_BINDER_SET_CONTEXT_MGR_EXT 0x4014620B  /* _IOW ('b', 11, flat_binder_object) size 20 */
#define LINUX_BINDER_THREAD_EXIT       0x40046208  /* _IOW ('b', 8, int32_t)            size 4  */
#define LINUX_BINDER_VERSION           0xC0186209  /* _IOWR('b', 9, binder_version)     size 24 */
#define LINUX_BINDER_SET_IDLE_TIMEOUT  0x40086203  /* _IOW ('b', 3, int64_t)            size 8  */
#define LINUX_BINDER_SET_IDLE_PRIORITY 0x40046206  /* _IOW ('b', 6, int32_t)            size 4  */

/*
 * Legacy M0 macros (kept for compat with binder-probe).
 */
#define BINDER_CURRENT_PROTOCOL_VERSION LINUX_BINDER_CURRENT_PROTOCOL_VERSION
#define BINDER_WRITE_READ        LINUX_BINDER_WRITE_READ
#define BINDER_SET_MAX_THREADS   LINUX_BINDER_SET_MAX_THREADS
#define BINDER_SET_CONTEXT_MGR   LINUX_BINDER_SET_CONTEXT_MGR
#define BINDER_SET_CONTEXT_MGR_EXT LINUX_BINDER_SET_CONTEXT_MGR_EXT
#define BINDER_THREAD_EXIT       LINUX_BINDER_THREAD_EXIT
#define BINDER_VERSION           LINUX_BINDER_VERSION
#define BINDER_SET_IDLE_TIMEOUT  LINUX_BINDER_SET_IDLE_TIMEOUT
#define BINDER_SET_IDLE_PRIORITY LINUX_BINDER_SET_IDLE_PRIORITY

struct binder_version {
    int32_t protocol_version;
    int32_t reserved[5];
};

#endif /* _FS_DEVFS_BINDER_H_ */