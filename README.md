# mSL/DevFS

[![C/C++ CI](https://github.com/mslxnu/devfs/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/mslxnu/devfs/actions/workflows/c-cpp.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-macOS-lightgrey.svg)
![Architecture](https://img.shields.io/badge/arch-arm64e%20%7C%20x86__64-blue.svg)

Linux-compatible character devices for macOS — starting with `/dev/binder`,
Android's IPC driver, so Android userspace running under mSL/NABI can talk.

One module of **mSL/XNU**, a modular macOS Subsystem for Linux.

> **Status: early.** The driver is complete enough to carry real binder traffic —
> transactions and replies, the object and handle translation between processes,
> the reference-counting protocol, death notifications, `poll()`, and the three
> Android contexts — and it builds clean for arm64e, x86_64 and universal. It has
> **not yet been run against a loaded kernel**: the load needs a password this
> repository's build cannot supply. `out/binder-probe` is the test that decides
> whether it works; see [Testing](#testing). Descriptor passing
> (`BINDER_TYPE_FD`) is deliberately incomplete — see
> [What macOS will not do](#what-macos-will-not-do).

## The larger project

| Piece | What it does | Where |
|-------|--------------|-------|
| **Syscall translation** | Linux system calls onto Darwin's, over `Hypervisor.framework` | [mSL/NABI](https://github.com/mslxnu/native_abi) |
| **Filesystem Hierarchy Standard** | Native Linux-style filesystem layout | [mSL/FHS](https://github.com/mslxnu/fhs) |
| **procfs** | `/proc`, as a native pseudo-filesystem | [mSL/ProcFS](https://github.com/mslxnu/procfs) |
| **sysfs** | `/sys`, as a native pseudo-filesystem | [mSL/SysFS](https://github.com/mslxnu/sysfs) |
| **devfs** | `/dev` driver add-ons for macOS | **this repository** |

The devfs pseudo-filesystem, mounted at `/dev`, is already present in XNU.
What it does not have is Linux-compatible character drivers such as binder.
SIP refuses a second devfs stacked over `/dev`, so these nodes are published
into XNU's own.

## What is binder?

Android's IPC. Not a socket: a transaction names an object, carries a parcel,
and the kernel rewrites every object reference inside that parcel so it means
the same thing to the receiving process — a local object becomes a handle, a
handle becomes a local object if it is going home. That translation, plus
reference counting that tells an object's owner when the last remote user lets
go, is what a `/dev/binder` is. `servicemanager`, every AIDL interface and every
HAL call in Android goes through it.

mSL/NABI's porting notes closed the Waydroid question with the observation that
`/dev/binder` "is a kernel driver with no macOS equivalent and nothing here
could supply one". This repository supplies one.

### Design

| Linux | Here |
|---|---|
| `drivers/android/binder.c` | `kext/binder_*.c` |
| `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder` | the same, via `devfs_make_node_clone` |
| binderfs mounted at `/dev/binderfs` | `/dev/binderfs/binder-control`, minting nodes beside itself |
| one `binder_proc` per `open()` | one per open, via a **cloning** device |
| `mmap()` of the driver | a client-registered arena (`BINDER_MSL_SET_ARENA`) |
| `BINDER_TYPE_FD` moves a descriptor | passed to a userspace broker |
| `/dev/ashmem` | not a device: Darwin shared memory, in the NABI shim |

## What macOS will not do

Three findings shaped everything above. All were verified against the XNU source
for the running kernel and the KPI export lists, not inferred:

**`mmap()` of a character device does not exist.** `bsd/kern/kern_mman.c`
answers `ENODEV` for every `VCHR` vnode, and `cdevsw.d_mmap` has no callers
anywhere in the kernel — the BSD page-number contract was dropped. Binder's
receive buffer is normally that mapping, so instead the client allocates the
region itself and registers it, and the driver copies each payload into it in
the receiving thread's own context, which is the one place a `copyout` to that
address is legal. It costs one extra copy per transaction; everything userspace
observes is unchanged.

**No kext can install a descriptor into a process.** `sys/file.h` exposes five
calls, all operating on the caller's own descriptors; `falloc`, `fp_lookup` and
`fp_drop` are exported to nothing. `BINDER_TYPE_FD` therefore travels with the
sender's descriptor number and the sender's pid (in a field Linux leaves as
padding), for a userspace broker to resolve over `SCM_RIGHTS`. `BINDER_TYPE_FDA`
is passed through to the same broker, which uses the sender's pid stamped into
the transaction header to translate the array.

**`poll()` cannot reach a third-party character device.** macOS implements
`poll()` over kqueue, and `filt_specattach` refuses a knote on a cdev whose
driver has not called `cdevsw_setkqueueok()` — which is absent from the SDK and
exported only through `com.apple.kpi.private`, a KPI the sibling modules
deliberately do not link. `poll()` therefore answers `POLLNVAL` without ever
consulting the driver. `select()` works, and so does kqueue when the knote
carries `NOTE_LOWAT` with a low-water mark of 1, which is the escape hatch in
that same check. This is not academic: Android's looper watches the binder fd
with epoll, mSL/NABI implements epoll over kqueue, so its shim sets
`NOTE_LOWAT` and gets exactly the readiness this driver reports.

**The ioctl direction bits are inverted.** BSD's `_IOW` is `IOC_IN`; Linux's is
the other bit, and XNU acts on them before the driver is called — a
Linux-numbered `_IOW` arrives as a buffer of zeroes. Clients send
`BINDER_CMD_HOST(cmd)`, which sets both; the driver matches on
`BINDER_CMD_KEY(cmd)`, which masks them off, and refuses data-carrying commands
that arrive without the input bit rather than acting on zeroes.

## Feature status

**Working (built, not yet run against a loaded kernel):**

- `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder` — three separate contexts,
  each with its own context manager, objects and handle numbering
- `/dev/binderfs/binder-control` with `BINDER_CTL_ADD`, minting further contexts
- `BINDER_VERSION` (protocol 8), `BINDER_SET_MAX_THREADS`,
  `BINDER_SET_CONTEXT_MGR`, `BINDER_SET_CONTEXT_MGR_EXT`, `BINDER_THREAD_EXIT`,
  `BINDER_GET_NODE_DEBUG_INFO`, `BINDER_GET_NODE_INFO_FOR_REF`,
  `BINDER_WRITE_READ`
- the full command streams: `BC_TRANSACTION`/`BC_REPLY` and their scatter-gather
  forms, `BC_FREE_BUFFER`, the four reference commands and their `_DONE`
  acknowledgements, the looper commands, death notification request and clear;
  `BR_TRANSACTION`/`BR_REPLY`, `BR_INCREFS`/`ACQUIRE`/`RELEASE`/`DECREFS`,
  `BR_DEAD_BINDER`, `BR_SPAWN_LOOPER`, `BR_FAILED_REPLY`/`BR_DEAD_REPLY`
- object translation across processes, including `BINDER_TYPE_PTR`
  scatter-gather buffers
- asynchronous transactions, one at a time per object, with the rest queued on it
- readiness that agrees with the read loop, through `select()` and through
  kqueue with `NOTE_LOWAT` (`poll()` is a platform limitation, above)
- teardown on close: objects declared dead, handles dropped, callers waiting on
  a reply told `BR_DEAD_REPLY`
- diagnostic counters under `sysctl devfs`

**Not implemented:**

- `BINDER_FREEZE` / `BINDER_GET_FROZEN_INFO`, oneway spam detection,
  `BINDER_GET_EXTENDED_ERROR` — accepted and recorded; frozen threads
  skip transactions in the read path
- the security-context transaction form (`BR_TRANSACTION_SEC_CTX`)
- `/dev/ashmem` — deliberately, see [doc/NABI-INTEGRATION.md](doc/NABI-INTEGRATION.md)
- fine-grained locking: the driver holds one mutex, by choice, and says why in
  `include/fs/devfs/binder_internal.h`

## Repository layout

```
mSL-DevFS/
├── doc/NABI-INTEGRATION.md   what mSL/NABI must do, and what Waydroid still needs
├── include/fs/devfs/
│   ├── binder.h              the Linux binder ABI, shared with userspace and NABI
│   └── binder_internal.h     the driver's own state, and why it is shaped that way
├── include/xnu/              vendored XNU private headers
├── kext/
│   ├── devfs.c               module start/stop, device major, diagnostic sysctls
│   ├── binder_dev.c          the cdevsw, the cloning nodes, the contexts, binderfs
│   ├── binder_proc.c         processes, threads, queues, the read and write loops
│   ├── binder_node.c         objects, handles, reference counting, death
│   ├── binder_txn.c          transactions and object translation
│   └── binder_alloc.c        the transaction arena
├── lib/                      git submodules (libkext, xnu)
├── out/                      build output (gitignored)
└── tools/
    ├── binder-probe.c        the functional test
    └── binder-abi-test.c     the ABI gate: every constant against its _IOC form
```

## Building

```bash
git submodule update --init lib/libkext
make                    # native arch (arm64e here)
make ARCH=x86_64
make ARCH=universal
```

Never build as root: `make install` only copies what `make` already built, so
every artifact stays owned by the invoking user.

## Loading

Third-party kexts on Apple Silicon need Reduced Security and a reboot before
they will load at all. With that done:

```bash
sudo make load
```

`make load` clears the kext staging area first — macOS caches third-party kexts
in the Auxiliary Kernel Collection, and a stale staged copy otherwise shadows a
freshly built one, which costs an afternoon the first time it happens.

```bash
sudo make unload
```

## Testing

The ABI gate runs as part of every build: `tools/binder-abi-test.c` recomputes
each ioctl number and command code from Linux's `_IOC` encoding applied to the
struct the ABI names, and fails the build if a literal disagrees. It exists
because the first draft of the header carried a fabricated 24-byte
`binder_version` (Linux's is 4 bytes), inverted direction bits on every `BC_`/
`BR_` code, and a constant size field — none of which is visible by inspection.

The functional test needs the kext loaded:

```bash
make check
```

It walks the protocol the way libbinder does: the device answers and reports
protocol 8; the arena is registered and its bounds enforced; a context manager
registers once and only once, per context; a oneway transaction to ourselves
completes and its payload arrives byte-for-byte inside the registered arena; a
synchronous call is served and replied to across two threads; `poll()` reports
readable exactly when there is work; and an object passed to a second process
arrives as a handle whose holder is told when its owner exits.

`make check` skips cleanly when nothing is loaded, so CI stays a compile gate.

The stages are independent and can be run one at a time, which is what you
want while the driver is still young enough to hang the machine:

```bash
./out/binder-probe --list
./out/binder-probe --stage oneway
```

Every line is also appended to `out/binder-probe.log`, flushed and `fsync`ed as
it is written. That matters more than it sounds: a driver bug here stops the
machine rather than the process, taking the terminal with it, and a synced log
survives the reboot. A log whose last line begins `..` names the operation that
never returned — the stage alone narrows a hang to a few hundred lines of
kernel code, and that line narrows it to one.

## Credits

The protocol, and much of the shape of the implementation, is Android's binder
driver — `drivers/android/binder.c`. Where this driver departs from it, the
reason is written down next to the departure.

## License

MIT. See [LICENSE](LICENSE).
