/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * binder.h
 *
 * The Linux binder ABI, on the wire, for /dev/binder and its sibling
 * contexts. Included by the kext, by the userspace tools, and (in the
 * LINUX_/l_ spelling its house style wants) by mSL/NABI.
 *
 * Everything below is Linux's, verbatim: the object types, the struct
 * layouts, the BC_/BR_ command streams and the ioctl encodings are the
 * ones drivers/android/binder.c uses, so a stock Android libbinder speaks
 * to this driver without knowing what is underneath it. Sizes are LP64;
 * a 32-bit guest uses the same 64-bit wire form, as Linux has since 3.15.
 *
 * Two things are NOT Linux's, and both are forced by macOS. They are
 * spelled out here rather than in a design document because they are the
 * first thing a reader of this header needs to know:
 *
 * 1. THE IOCTL DIRECTION BITS ARE INVERTED. BSD's _IOW is IOC_IN
 *    (0x80000000) and its _IOR is IOC_OUT (0x40000000); Linux numbers them
 *    the other way round. XNU reads those bits itself, before the driver
 *    is called (bsd/kern/sys_generic.c, ioctl()): IOC_IN copies the
 *    argument in, IOC_OUT zeroes the buffer and copies it back out after.
 *    So a command Linux encodes as _IOW - BINDER_SET_CONTEXT_MGR_EXT, say -
 *    reaches the driver as a buffer of zeroes, with the caller's data never
 *    copied. A client must therefore issue every binder ioctl with BOTH
 *    direction bits set, which is what BINDER_CMD_HOST() does; the driver
 *    in turn switches on BINDER_CMD_KEY(), which masks the direction bits
 *    off entirely, so it accepts either spelling and cannot be broken by
 *    this again. NABI's shim applies BINDER_CMD_HOST to the guest's number.
 *
 * 2. BINDER_TYPE_FD CANNOT BE HONOURED IN THE KERNEL. No KPI lets a kext
 *    install a descriptor into a process (sys/file.h exposes five calls,
 *    all operating on the caller's own fds; falloc/fp_lookup/fp_drop are in
 *    no export list). The driver validates fd objects, leaves the sender's
 *    descriptor number in `fd`, and stamps the sender's pid into the
 *    otherwise-unused `cookie` - enough for a userspace broker to move the
 *    real descriptor over SCM_RIGHTS. See doc/NABI-INTEGRATION.md.
 *
 * The mSL private ioctl range (nr >= 0xE0 under type 'b') carries the one
 * extension the design needs: the client-registered transaction arena,
 * which stands in for the mmap() of /dev/binder that XNU has no way to
 * serve (kern_mman.c returns ENODEV for every character-device vnode).
 */

#ifndef _FS_DEVFS_BINDER_H_
#define _FS_DEVFS_BINDER_H_

#include <stdint.h>

/*
 * Compile-time size check. Every wire struct below carries one: the ioctl
 * encodings embed sizeof(), so a struct that drifts silently renumbers the
 * ABI. A file-scope typedef, rather than _Static_assert, so the same header
 * compiles under the kext's -std=gnu99 and in userspace without warnings.
 */
#define BINDER_ABI_ASSERT(tag, cond) \
	typedef char binder_abi_assert_##tag[(cond) ? 1 : -1]

#pragma mark -
#pragma mark Protocol Version

/*
 * What BINDER_VERSION reports. 8 is the 64-bit protocol every Android
 * since 5.0 speaks; 7 was the 32-bit one, which this driver does not serve.
 */
#define BINDER_CURRENT_PROTOCOL_VERSION 8

/* The revision of the mSL extensions (BINDER_MSL_*), reported by
 * BINDER_MSL_ABI_VERSION. Bumped when an extension changes shape. */
#define BINDER_MSL_ABI_CURRENT 1

#pragma mark -
#pragma mark Ioctl Encoding

/*
 * Linux's _IOC encoding, written out here so the constants below can be
 * checked against it rather than trusted (tools/binder-abi-test.c does
 * exactly that). Note these are LINUX's direction bits, not BSD's.
 */
#define BINDER_IOC_NONE   0u
#define BINDER_IOC_WRITE  1u
#define BINDER_IOC_READ   2u
#define BINDER_IOC(dir, type, nr, size) \
	((uint32_t)(((dir) << 30) | ((size) << 16) | ((type) << 8) | (nr)))

/*
 * Linux command -> the number a client must actually send on macOS: both
 * direction bits set, so XNU copies the argument in AND back out. See the
 * file comment.
 */
#define BINDER_CMD_HOST(cmd) ((uint32_t)((cmd) | 0xC0000000u))

/*
 * The direction-independent identity of a command: type, nr and size. What
 * the driver switches on, so either spelling is accepted.
 */
#define BINDER_CMD_KEY(cmd)  ((uint32_t)((cmd) & 0x3FFFFFFFu))

#pragma mark -
#pragma mark Object Types

#define B_PACK_CHARS(c1, c2, c3, c4) \
	((((uint32_t)(c1) << 24) | ((uint32_t)(c2) << 16) | \
	  ((uint32_t)(c3) << 8) | (uint32_t)(c4)))
#define B_TYPE_LARGE 0x85

enum {
	BINDER_TYPE_BINDER      = B_PACK_CHARS('s', 'b', '*', B_TYPE_LARGE),
	BINDER_TYPE_WEAK_BINDER = B_PACK_CHARS('w', 'b', '*', B_TYPE_LARGE),
	BINDER_TYPE_HANDLE      = B_PACK_CHARS('s', 'h', '*', B_TYPE_LARGE),
	BINDER_TYPE_WEAK_HANDLE = B_PACK_CHARS('w', 'h', '*', B_TYPE_LARGE),
	BINDER_TYPE_FD          = B_PACK_CHARS('f', 'd', '*', B_TYPE_LARGE),
	BINDER_TYPE_FDA         = B_PACK_CHARS('f', 'd', 'a', B_TYPE_LARGE),
	BINDER_TYPE_PTR         = B_PACK_CHARS('p', 't', '*', B_TYPE_LARGE),
};

/* flat_binder_object.flags */
#define FLAT_BINDER_FLAG_PRIORITY_MASK      0x00ff
#define FLAT_BINDER_FLAG_ACCEPTS_FDS        0x0100
#define FLAT_BINDER_FLAG_SCHED_POLICY_SHIFT 9
#define FLAT_BINDER_FLAG_SCHED_POLICY_MASK  (3u << FLAT_BINDER_FLAG_SCHED_POLICY_SHIFT)
#define FLAT_BINDER_FLAG_INHERIT_RT         0x0800
#define FLAT_BINDER_FLAG_TXN_SECURITY_CTX   0x1000

/* binder_buffer_object.flags */
#define BINDER_BUFFER_FLAG_HAS_PARENT       0x01

#pragma mark -
#pragma mark Wire Types

typedef uint64_t binder_size_t;
typedef uint64_t binder_uintptr_t;

struct binder_object_header {
	uint32_t type;
};
BINDER_ABI_ASSERT(object_header, sizeof(struct binder_object_header) == 4);

/*
 * A local object (BINDER_TYPE_BINDER/WEAK_BINDER, `binder` + `cookie` are
 * the owner's pointers) or a handle to somebody else's (BINDER_TYPE_HANDLE/
 * WEAK_HANDLE, `handle` is this process's descriptor for it). The driver
 * rewrites one into the other as the object crosses a process boundary.
 */
struct flat_binder_object {
	struct binder_object_header hdr;
	uint32_t flags;
	union {
		binder_uintptr_t binder;
		uint32_t handle;
	};
	binder_uintptr_t cookie;
};
BINDER_ABI_ASSERT(flat_binder_object, sizeof(struct flat_binder_object) == 24);

/*
 * A descriptor. `fd` is the sender's descriptor number and `cookie` the
 * sender's pid on the way out - the mSL deviation described at the top of
 * this file. On Linux `cookie` is unused padding, so a receiver that
 * ignores it behaves exactly as it would there.
 */
struct binder_fd_object {
	struct binder_object_header hdr;
	uint32_t pad_flags;
	union {
		binder_uintptr_t pad_binder;
		uint32_t fd;
	};
	binder_uintptr_t cookie;
};
BINDER_ABI_ASSERT(fd_object, sizeof(struct binder_fd_object) == 24);

/* A scatter-gather buffer: payload carried beside the parcel rather than
 * inside it, so large parcels are not copied twice. */
struct binder_buffer_object {
	struct binder_object_header hdr;
	uint32_t flags;
	binder_uintptr_t buffer;
	binder_size_t length;
	binder_size_t parent;
	binder_size_t parent_offset;
};
BINDER_ABI_ASSERT(buffer_object, sizeof(struct binder_buffer_object) == 40);

/* An array of descriptors living inside a scatter-gather buffer. */
struct binder_fd_array_object {
	struct binder_object_header hdr;
	uint32_t pad;
	binder_size_t num_fds;
	binder_size_t parent;
	binder_size_t parent_offset;
};
BINDER_ABI_ASSERT(fd_array_object, sizeof(struct binder_fd_array_object) == 32);

struct binder_write_read {
	binder_size_t write_size;
	binder_size_t write_consumed;
	binder_uintptr_t write_buffer;
	binder_size_t read_size;
	binder_size_t read_consumed;
	binder_uintptr_t read_buffer;
};
BINDER_ABI_ASSERT(write_read, sizeof(struct binder_write_read) == 48);

struct binder_version {
	int32_t protocol_version;
};
BINDER_ABI_ASSERT(version, sizeof(struct binder_version) == 4);

struct binder_node_debug_info {
	binder_uintptr_t ptr;
	binder_uintptr_t cookie;
	uint32_t has_strong_ref;
	uint32_t has_weak_ref;
};
BINDER_ABI_ASSERT(node_debug_info, sizeof(struct binder_node_debug_info) == 24);

struct binder_node_info_for_ref {
	uint32_t handle;
	uint32_t strong_count;
	uint32_t weak_count;
	uint32_t reserved1;
	uint32_t reserved2;
	uint32_t reserved3;
};
BINDER_ABI_ASSERT(node_info_for_ref, sizeof(struct binder_node_info_for_ref) == 24);

struct binder_freeze_info {
	uint32_t pid;
	uint32_t enable;
	uint32_t timeout_ms;
};
BINDER_ABI_ASSERT(freeze_info, sizeof(struct binder_freeze_info) == 12);

struct binder_frozen_status_info {
	uint32_t pid;
	uint32_t sync_recv;
	uint32_t async_recv;
};
BINDER_ABI_ASSERT(frozen_status_info, sizeof(struct binder_frozen_status_info) == 12);

struct binder_extended_error {
	uint32_t id;
	uint32_t command;
	int32_t param;
};
BINDER_ABI_ASSERT(extended_error, sizeof(struct binder_extended_error) == 12);

struct binder_transaction_data {
	union {
		binder_uintptr_t ptr;   /* target object, on the way in  */
		uint32_t handle;        /* target handle, on the way out */
	} target;
	binder_uintptr_t cookie;
	uint32_t code;
	uint32_t flags;
	int32_t sender_pid;
	uint32_t sender_euid;
	binder_size_t data_size;
	binder_size_t offsets_size;
	union {
		struct {
			binder_uintptr_t buffer;
			binder_uintptr_t offsets;
		} ptr;
		uint8_t buf[8];
	} data;
};
BINDER_ABI_ASSERT(transaction_data, sizeof(struct binder_transaction_data) == 64);

struct binder_transaction_data_secctx {
	struct binder_transaction_data transaction_data;
	binder_uintptr_t secctx;
};
BINDER_ABI_ASSERT(transaction_data_secctx,
    sizeof(struct binder_transaction_data_secctx) == 72);

struct binder_transaction_data_sg {
	struct binder_transaction_data transaction_data;
	binder_size_t buffers_size;
};
BINDER_ABI_ASSERT(transaction_data_sg,
    sizeof(struct binder_transaction_data_sg) == 72);

struct binder_ptr_cookie {
	binder_uintptr_t ptr;
	binder_uintptr_t cookie;
};
BINDER_ABI_ASSERT(ptr_cookie, sizeof(struct binder_ptr_cookie) == 16);

struct binder_handle_cookie {
	uint32_t handle;
	binder_uintptr_t cookie;
} __attribute__((packed));
BINDER_ABI_ASSERT(handle_cookie, sizeof(struct binder_handle_cookie) == 12);

struct binder_pri_desc {
	int32_t priority;
	uint32_t desc;
};
BINDER_ABI_ASSERT(pri_desc, sizeof(struct binder_pri_desc) == 8);

struct binder_pri_ptr_cookie {
	int32_t priority;
	binder_uintptr_t ptr;
	binder_uintptr_t cookie;
};
BINDER_ABI_ASSERT(pri_ptr_cookie, sizeof(struct binder_pri_ptr_cookie) == 24);

/* binder_transaction_data.flags */
enum transaction_flags {
	TF_ONE_WAY     = 0x01,  /* asynchronous, no reply expected     */
	TF_ROOT_OBJECT = 0x04,  /* the object is the root of a service */
	TF_STATUS_CODE = 0x08,  /* the payload is a 32-bit status      */
	TF_ACCEPT_FDS  = 0x10,  /* the reply may carry descriptors     */
	TF_CLEAR_BUF   = 0x20,  /* wipe the buffer when it is freed    */
	TF_UPDATE_TXN  = 0x40,  /* supersede a pending oneway txn      */
};

#pragma mark -
#pragma mark Ioctls

/*
 * Linux's numbers, verbatim. A client sends BINDER_CMD_HOST(x); the driver
 * matches BINDER_CMD_KEY(x). The trailing comment gives the derivation, in
 * the mSL/NABI house form.
 */
#define BINDER_WRITE_READ                   0xC0306201u  /* _IOWR('b',  1, struct binder_write_read)          */
#define BINDER_SET_IDLE_TIMEOUT             0x40086203u  /* _IOW ('b',  3, int64_t)                           */
#define BINDER_SET_MAX_THREADS              0x40046205u  /* _IOW ('b',  5, uint32_t)                          */
#define BINDER_SET_IDLE_PRIORITY            0x40046206u  /* _IOW ('b',  6, int32_t)                           */
#define BINDER_SET_CONTEXT_MGR              0x40046207u  /* _IOW ('b',  7, int32_t)                           */
#define BINDER_THREAD_EXIT                  0x40046208u  /* _IOW ('b',  8, int32_t)                           */
#define BINDER_VERSION                      0xC0046209u  /* _IOWR('b',  9, struct binder_version)             */
#define BINDER_GET_NODE_DEBUG_INFO          0xC018620Bu  /* _IOWR('b', 11, struct binder_node_debug_info)     */
#define BINDER_GET_NODE_INFO_FOR_REF        0xC018620Cu  /* _IOWR('b', 12, struct binder_node_info_for_ref)   */
#define BINDER_SET_CONTEXT_MGR_EXT          0x4018620Du  /* _IOW ('b', 13, struct flat_binder_object)         */
#define BINDER_FREEZE                       0x400C620Eu  /* _IOW ('b', 14, struct binder_freeze_info)         */
#define BINDER_GET_FROZEN_INFO              0xC00C620Fu  /* _IOWR('b', 15, struct binder_frozen_status_info)  */
#define BINDER_ENABLE_ONEWAY_SPAM_DETECTION 0x40046210u  /* _IOW ('b', 16, uint32_t)                          */
#define BINDER_GET_EXTENDED_ERROR           0xC00C6211u  /* _IOWR('b', 17, struct binder_extended_error)      */

/*
 * binderfs. On Linux this is a filesystem whose binder-control node mints
 * new devices; here the same ioctl is served by /dev/binderfs/binder-control
 * and mints a devfs node beside it.
 */
#define BINDERFS_MAX_NAME 255

struct binderfs_device {
	char name[BINDERFS_MAX_NAME + 1];
	uint32_t major;
	uint32_t minor;
};
BINDER_ABI_ASSERT(binderfs_device, sizeof(struct binderfs_device) == 264);

#define BINDER_CTL_ADD                      0xC1086201u  /* _IOWR('b',  1, struct binderfs_device)            */

/*
 * mSL extensions. nr >= 0xE0 under type 'b' - a range upstream has never
 * used and, being at the top of the byte, is not on its way to using.
 *
 * BINDER_MSL_SET_ARENA registers the memory the driver may place incoming
 * transaction payloads in. It replaces mmap(): the client allocates the
 * region (MAP_ANON|MAP_SHARED, page-aligned) and the driver allocates
 * within it, handing back offsets exactly as Linux hands back offsets into
 * its own mapping, so BC_FREE_BUFFER and the buffer pointers in
 * BR_TRANSACTION/BR_REPLY carry their ordinary meaning. It may be issued
 * once per open, before any transaction.
 */
struct binder_msl_arena {
	uint64_t addr;   /* base of the client's region        */
	uint64_t size;   /* its length in bytes, a multiple of the page size */
};
BINDER_ABI_ASSERT(msl_arena, sizeof(struct binder_msl_arena) == 16);

#define BINDER_MSL_SET_ARENA                0xC01062E0u  /* _IOWR('b', 0xE0, struct binder_msl_arena)         */
#define BINDER_MSL_ABI_VERSION              0xC00462E1u  /* _IOWR('b', 0xE1, uint32_t)                        */

/* Linux caps a binder mapping at 4MB; Android asks for 1MB - 8KB. */
#define BINDER_MSL_ARENA_MIN (128u * 1024u)
#define BINDER_MSL_ARENA_MAX (4u * 1024u * 1024u)

#pragma mark -
#pragma mark Driver Return Protocol (BR_*)

/*
 * What the driver writes into the read buffer. Each is a 32-bit command
 * followed by the payload its encoding names.
 */
enum binder_driver_return_protocol {
	BR_ERROR                         = 0x80047200u, /* _IOR('r',  0, int32_t)                              */
	BR_OK                            = 0x00007201u, /* _IO ('r',  1)                                       */
	BR_TRANSACTION_SEC_CTX           = 0x80487202u, /* _IOR('r',  2, struct binder_transaction_data_secctx)*/
	BR_TRANSACTION                   = 0x80407202u, /* _IOR('r',  2, struct binder_transaction_data)       */
	BR_REPLY                         = 0x80407203u, /* _IOR('r',  3, struct binder_transaction_data)       */
	BR_ACQUIRE_RESULT                = 0x80047204u, /* _IOR('r',  4, int32_t)                              */
	BR_DEAD_REPLY                    = 0x00007205u, /* _IO ('r',  5)                                       */
	BR_TRANSACTION_COMPLETE          = 0x00007206u, /* _IO ('r',  6)                                       */
	BR_INCREFS                       = 0x80107207u, /* _IOR('r',  7, struct binder_ptr_cookie)             */
	BR_ACQUIRE                       = 0x80107208u, /* _IOR('r',  8, struct binder_ptr_cookie)             */
	BR_RELEASE                       = 0x80107209u, /* _IOR('r',  9, struct binder_ptr_cookie)             */
	BR_DECREFS                       = 0x8010720Au, /* _IOR('r', 10, struct binder_ptr_cookie)             */
	BR_ATTEMPT_ACQUIRE               = 0x8018720Bu, /* _IOR('r', 11, struct binder_pri_ptr_cookie)         */
	BR_NOOP                          = 0x0000720Cu, /* _IO ('r', 12)                                       */
	BR_SPAWN_LOOPER                  = 0x0000720Du, /* _IO ('r', 13)                                       */
	BR_FINISHED                      = 0x0000720Eu, /* _IO ('r', 14)                                       */
	BR_DEAD_BINDER                   = 0x8008720Fu, /* _IOR('r', 15, binder_uintptr_t)                     */
	BR_CLEAR_DEATH_NOTIFICATION_DONE = 0x80087210u, /* _IOR('r', 16, binder_uintptr_t)                     */
	BR_FAILED_REPLY                  = 0x00007211u, /* _IO ('r', 17)                                       */
	BR_FROZEN_REPLY                  = 0x00007212u, /* _IO ('r', 18)                                       */
	BR_ONEWAY_SPAM_SUSPECT           = 0x00007213u, /* _IO ('r', 19)                                       */
	BR_TRANSACTION_PENDING_FROZEN    = 0x00007214u, /* _IO ('r', 20)                                       */
};

#pragma mark -
#pragma mark Driver Command Protocol (BC_*)

/* What the client writes into the write buffer. */
enum binder_driver_command_protocol {
	BC_TRANSACTION                = 0x40406300u, /* _IOW('c',  0, struct binder_transaction_data)    */
	BC_REPLY                      = 0x40406301u, /* _IOW('c',  1, struct binder_transaction_data)    */
	BC_ACQUIRE_RESULT             = 0x40046302u, /* _IOW('c',  2, int32_t)                           */
	BC_FREE_BUFFER                = 0x40086303u, /* _IOW('c',  3, binder_uintptr_t)                  */
	BC_INCREFS                    = 0x40046304u, /* _IOW('c',  4, uint32_t)                          */
	BC_ACQUIRE                    = 0x40046305u, /* _IOW('c',  5, uint32_t)                          */
	BC_RELEASE                    = 0x40046306u, /* _IOW('c',  6, uint32_t)                          */
	BC_DECREFS                    = 0x40046307u, /* _IOW('c',  7, uint32_t)                          */
	BC_INCREFS_DONE               = 0x40106308u, /* _IOW('c',  8, struct binder_ptr_cookie)          */
	BC_ACQUIRE_DONE               = 0x40106309u, /* _IOW('c',  9, struct binder_ptr_cookie)          */
	BC_ATTEMPT_ACQUIRE            = 0x4008630Au, /* _IOW('c', 10, struct binder_pri_desc)            */
	BC_REGISTER_LOOPER            = 0x0000630Bu, /* _IO ('c', 11)                                    */
	BC_ENTER_LOOPER               = 0x0000630Cu, /* _IO ('c', 12)                                    */
	BC_EXIT_LOOPER                = 0x0000630Du, /* _IO ('c', 13)                                    */
	BC_REQUEST_DEATH_NOTIFICATION = 0x400C630Eu, /* _IOW('c', 14, struct binder_handle_cookie)       */
	BC_CLEAR_DEATH_NOTIFICATION   = 0x400C630Fu, /* _IOW('c', 15, struct binder_handle_cookie)       */
	BC_DEAD_BINDER_DONE           = 0x40086310u, /* _IOW('c', 16, binder_uintptr_t)                  */
	BC_TRANSACTION_SG             = 0x40486311u, /* _IOW('c', 17, struct binder_transaction_data_sg) */
	BC_REPLY_SG                   = 0x40486312u, /* _IOW('c', 18, struct binder_transaction_data_sg) */
};

#pragma mark -
#pragma mark NABI Spelling

/*
 * mSL/NABI prefixes every Linux constant it carries, to keep them apart
 * from Darwin's own. These aliases let its translation layer include this
 * header directly instead of keeping a second copy in step with it.
 */
#define LINUX_BINDER_CURRENT_PROTOCOL_VERSION BINDER_CURRENT_PROTOCOL_VERSION
#define LINUX_BINDER_WRITE_READ               BINDER_WRITE_READ
#define LINUX_BINDER_SET_IDLE_TIMEOUT         BINDER_SET_IDLE_TIMEOUT
#define LINUX_BINDER_SET_MAX_THREADS          BINDER_SET_MAX_THREADS
#define LINUX_BINDER_SET_IDLE_PRIORITY        BINDER_SET_IDLE_PRIORITY
#define LINUX_BINDER_SET_CONTEXT_MGR          BINDER_SET_CONTEXT_MGR
#define LINUX_BINDER_THREAD_EXIT              BINDER_THREAD_EXIT
#define LINUX_BINDER_VERSION                  BINDER_VERSION
#define LINUX_BINDER_GET_NODE_DEBUG_INFO      BINDER_GET_NODE_DEBUG_INFO
#define LINUX_BINDER_GET_NODE_INFO_FOR_REF    BINDER_GET_NODE_INFO_FOR_REF
#define LINUX_BINDER_SET_CONTEXT_MGR_EXT      BINDER_SET_CONTEXT_MGR_EXT
#define LINUX_BINDER_FREEZE                   BINDER_FREEZE
#define LINUX_BINDER_GET_FROZEN_INFO          BINDER_GET_FROZEN_INFO
#define LINUX_BINDER_GET_EXTENDED_ERROR       BINDER_GET_EXTENDED_ERROR
#define LINUX_BINDER_CTL_ADD                  BINDER_CTL_ADD
#define LINUX_BINDER_MSL_SET_ARENA            BINDER_MSL_SET_ARENA
#define LINUX_BINDER_MSL_ABI_VERSION          BINDER_MSL_ABI_VERSION

#endif /* _FS_DEVFS_BINDER_H_ */
