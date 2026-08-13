# Running binder under mSL/NABI

This document is the other half of the work: the kext publishes `/dev/binder`,
and this is what mSL/NABI has to do so a Linux guest can use it. Nothing here
has been applied to the NABI tree — the changes are specified against real call
sites, with the code that does the awkward parts written out, so they can be
reviewed and landed there deliberately.

It is a specification rather than a patch on purpose. NABI's `src/fs/fs.c` was
being edited while this was written (`chroot` and `pivot_root` landed in its
working tree), and a diff against a moving file is worth less than a precise
description of where the hooks go.

---

## Why NABI has to be involved at all

A guest's `open("/dev/binder")` already works: `/dev` is an unconditional host
passthrough (`host_passthrough[]`, `src/fs/fs.c`), so the guest reaches the real
device node this kext creates. Everything after that needs help, for three
separate reasons.

**1. The pointers are in the wrong address space.** NABI runs the guest with its
own page tables; a guest address is meaningless to the kernel, which copies from
the *nabi process's* address space. `struct binder_write_read` carries two
pointers, and the command stream inside them carries more. Every one has to be
rewritten through `guest_to_host()` on the way down.

**2. The ioctl direction bits are inverted on macOS.** BSD's `_IOW` is `IOC_IN`;
Linux's is the opposite bit. XNU acts on those bits *before* the driver sees the
call — a Linux-numbered `_IOW` arrives as a zeroed buffer. The shim must send
`BINDER_CMD_HOST(cmd)`, which sets both bits. The driver refuses commands that
carry data without the input bit rather than acting on zeroes, so getting this
wrong fails loudly.

**3. There is no mmap.** XNU refuses `mmap()` on character devices outright
(`bsd/kern/kern_mman.c` answers `ENODEV` for every `VCHR` vnode). The driver
therefore takes a client-allocated region — `BINDER_MSL_SET_ARENA` — and the
shim provides one that the guest and the host both see.

---

## The four changes

### 1. Recognise the binder fd at open

`user_openat()` already has the fd and the resolved path in hand where it calls
`fcntl(fd, F_GETPATH, opath)` before `register_fd()`. A binder fd is a character
device whose name is `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder` or
anything under `/dev/binderfs/`.

`struct file` has no private-data slot and one `file_operations` table serves
every descriptor, so per-fd state goes in a side table keyed by the host fd —
the pattern eventfd and epoll already use (`eventfd_lookup(file->fd)` at the top
of `darwinfs_readv`/`darwinfs_writev`). Add the matching teardown to
`user_close()` beside `epoll_close()` and friends.

```c
/* src/fs/binder.c */
KHASH_MAP_INIT_INT(binderfd, struct binder_state *)

struct binder_state {
  int      fd;
  void    *arena;        /* host mapping, shared with the guest */
  gaddr_t  arena_gaddr;  /* the same pages, as the guest sees them */
  size_t   arena_size;
};
```

### 2. Intercept the ioctl

Call `binder_ioctl()` from the top of `darwinfs_ioctl()`, before its switch —
unknown commands there currently `warnk` and return `-LINUX_EPERM`, which is
what a binder ioctl would hit today.

The command number is passed through with both direction bits set. The only
command needing real work is `BINDER_WRITE_READ`; the rest are fixed-size
structs with no embedded pointers, so they are a `copy_from_user`, an `ioctl`,
and a `copy_to_user` back.

### 3. Translate the command streams

This is the whole of the difficulty, and it is bounded: the driver dereferences
exactly three kinds of user pointer, and the shim rewrites those and nothing
else.

**On the way down**, for each `BC_TRANSACTION`, `BC_REPLY`, `BC_TRANSACTION_SG`
or `BC_REPLY_SG` in the write buffer:

| Field | What to do |
|---|---|
| `data.ptr.buffer` | `guest_to_host()` |
| `data.ptr.offsets` | `guest_to_host()` |
| each `binder_buffer_object.buffer` (a `BINDER_TYPE_PTR` object named by the offsets array) | `guest_to_host()` |

The parcel itself is *not* copied or rewritten: the guest's bytes are already
host bytes at a different address, and the objects inside it are translated by
the driver, not by the shim.

**On the way up**, for each `BR_TRANSACTION`, `BR_TRANSACTION_SEC_CTX` or
`BR_REPLY` in the read buffer, the driver has filled in addresses inside the
registered arena, which is host memory:

| Field | What to do |
|---|---|
| `data.ptr.buffer` | host → guest: `arena_gaddr + (ptr - (uintptr_t)arena)` |
| `data.ptr.offsets` | the same |
| each `BINDER_TYPE_PTR` object's `buffer`, inside the delivered parcel | the same |

`BC_FREE_BUFFER` runs the first mapping again, since the guest hands back the
pointer it was given.

A reverse map is worth writing generally — NABI has `guest_to_host()` but no
`host_to_guest()`, and the region list carries both addresses
(`struct mm_region.haddr` / `.gaddr`), so it is about ten lines. For binder it
is not needed: the arena is one region whose base is known.

### 4. Provide the arena instead of mmap

The guest's `mmap(NULL, size, PROT_READ, MAP_PRIVATE, binder_fd, 0)` must not
reach `do_mmap()`'s file path — on arm64 that falls through to `arena_alloc()`
plus a `pread()`, which a character device cannot serve, and the mapping would
be a private copy with no relation to the driver even if it could.

Intercept it: when the fd is a binder fd, allocate the region and register it.

```c
/* Host memory the guest also sees: mmap gives 16 KiB alignment, which is
 * what vmm_mmap asserts, and MAP_SHARED keeps one set of pages. */
void *host = mmap(NULL, len, PROT_READ | PROT_WRITE,
                  MAP_ANON | MAP_SHARED, -1, 0);
gaddr_t gaddr = /* place it in the guest's mmap arena */;
vmm_mmap(gaddr, len, prot, host);

struct binder_msl_arena arena = {
  .addr = (uint64_t)(uintptr_t)host,   /* the driver copies to HOST addresses */
  .size = len,
};
ioctl(fd, BINDER_CMD_HOST(BINDER_MSL_SET_ARENA), &arena);
```

The driver copies each payload into `host`; the guest reads it at `gaddr`,
because they are the same pages. Record both so step 3 can convert between them.

### 4b. Set NOTE_LOWAT when registering a binder fd with kqueue

NABI implements epoll over kqueue, and a guest's looper will register the
binder fd for readability. A plain `EVFILT_READ` knote on this device is
refused with `EINVAL` — macOS will not attach one to a third-party character
device whose driver has not called `cdevsw_setkqueueok()`, which is private
KPI. The check that refuses it accepts a knote carrying `NOTE_LOWAT` with a
low-water mark of 1:

```c
EV_SET(&kev, host_fd, EVFILT_READ, EV_ADD, NOTE_LOWAT, 1, udata);
```

With that, readiness is exact: the driver reports work through the same
predicate its read loop uses, so a looper never spins and never sleeps through
a pending transaction. Without it, `epoll_ctl(EPOLL_CTL_ADD)` fails and the
guest concludes binder is broken.

`select()` needs nothing special. `poll()` cannot be made to work at all from
outside the kernel — it answers `POLLNVAL` — so a guest that polls the binder
fd directly is a case the shim has to translate rather than pass through.

### 5. Do not let NABI's own signals surface as EINTR

A thread parked in `BINDER_WRITE_READ` is interruptible by design. NABI takes
signals of its own that mean nothing to the guest, and
`RETRY_ON_RESTARTABLE_EINTR` (`src/fs/fs.c`) exists for exactly this: it retries
when the pending signal is not one the guest asked to see. Wrap the blocking
ioctl in it, as the termios paths do.

---

## Descriptors: the part the kernel cannot do

`BINDER_TYPE_FD` asks the kernel to move an open file from one process to
another. No KPI on macOS can: `sys/file.h` exposes five calls, all operating on
the caller's own descriptors, and `falloc`/`fp_lookup`/`fp_drop` are exported to
nothing. So the driver does what it can and leaves the rest to userspace:

- it validates the object, refuses it if the receiver did not set
  `FLAT_BINDER_FLAG_ACCEPTS_FDS`, and passes it through with the sender's
  descriptor number in `fd` and **the sender's pid in `cookie`** (padding on
  Linux, so a receiver that ignores it behaves exactly as it would there);
- `BINDER_TYPE_FDA`, an array of descriptors, is refused outright — there is
  nowhere in that object to record a sender per entry, and delivering an array
  of numbers that mean nothing in the receiver is worse than failing.

The broker that closes the gap is a NABI-side job, and NABI already has the
mechanism: `SCM_RIGHTS` over `AF_UNIX`, translated in both directions
(`src/net/net.c`). The shape:

1. Every nabi instance connects to one rendezvous socket (in the runtime
   directory, alongside the other per-instance state).
2. A sender that sees `BINDER_TYPE_FD` leave in a transaction registers
   `(pid, fd)` with the broker and sends the descriptor over `SCM_RIGHTS`.
3. A receiver that sees one arrive asks the broker for `(cookie, fd)`, receives
   the descriptor, and rewrites the object's `fd` to its own number before the
   guest sees it.

The guest's `libbinder` is untouched: the substitution happens at the
`BINDER_TYPE_FD` boundary, which is where the ABI puts descriptors.

---

## ashmem needs no kernel at all

`/dev/ashmem` is worth a paragraph because it is the other device Waydroid
expects, and because the right answer is *not* a driver.

Everything ashmem does is `mmap`: a process opens it, names and sizes a region,
maps it, and passes the descriptor to somebody else. A character device is the
one thing on macOS that cannot be mapped, so a kext could serve the ioctls and
still be useless. Darwin's own shared memory does the whole job:

| ashmem | Darwin |
|---|---|
| `open("/dev/ashmem")` | `shm_open()` an unlinked name, or a temporary file |
| `ASHMEM_SET_NAME` | remembered in the shim |
| `ASHMEM_SET_SIZE` | `ftruncate()` |
| `mmap()` | an ordinary `mmap` of that descriptor |
| passing the fd | `SCM_RIGHTS`, already translated |
| `ASHMEM_SET_PROT_MASK` | `mprotect` at map time |
| `ASHMEM_PIN` / `UNPIN` / `GET_PIN_STATUS` | answer "pinned"; Darwin does not discard |
| `ASHMEM_PURGE_ALL_CACHES` | answer 0 |

So the kext publishes no `/dev/ashmem`, and the shim implements it in userspace.
Android 11 — what Waydroid ships — prefers `memfd` for most of this anyway, and
NABI has `memfd` already, without sealing (`src/fs/memfd.c` says so plainly).

---

## What is still missing for Waydroid

Binder was one wall. This is an honest list of the others, from reading NABI's
tree rather than from the older prose in `PORTING-arm64.md`, which predates its
own `chroot`/`pivot_root` work:

| Gap | Where | Notes |
|---|---|---|
| `mknod` for character devices | `src/fs/fs.c`, the `mknod` case handles only `S_IFIFO` | LXC populates a container `/dev` this way |
| `clone(CLONE_NEWNS\|NEWPID\|NEWNET)` | `src/proc/fork.c`, not in the accepted-flags set | `unshare`/`setns` work; `lxc-start` clones |
| backed `devtmpfs` / `devpts` mounts | `src/fs/mount.c`, recorded with no host directory | a container mounting its own `/dev` gets nothing |
| cgroup controllers | `src/proc/cgroup.c` — hierarchy real, controllers deliberately absent | `lxc-start` writes `memory.max`, `pids.max` |
| `memfd` sealing | `src/fs/memfd.c`, `F_ADD_SEALS` answers `EINVAL` | Android seals its shared buffers |
| per-thread signals | `src/proc/process.c` — `tgkill` is process-wide | libbinder's pool interrupts specific threads |
| `binderfs` as a mount type | `src/fs/mount.c`'s `backing_for_type` | the nodes exist at `/dev/binderfs/`; a guest that *mounts* binderfs needs the type recorded |

`chroot` and `pivot_root` are no longer on this list — they are implemented in
NABI's working tree, which is worth saying because the older documentation still
names them as the reason Waydroid is unreachable.
