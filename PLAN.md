# mSL/DevFS — implementation plan

`/dev/binder` (and friends) for Android/Waydroid userspace running under mSL/NABI.

Status: **plan**. Nothing is implemented yet; this repository is an empty git tree.
This document is the agreed shape of the work. It supersedes any earlier
discussion in the session that created it.

---

## 1. Goal

Publish Linux-compatible character devices — first and foremost `/dev/binder`,
Android's kernel IPC driver — so that Android userspace running under mSL/NABI
(the Linux-syscall translator in `/Users/sunneva/Development/mSL-XNU/mSL-NABI`)
can actually use binder. Without it, Waydroid is unreachable: it is Android in
an LXC container talking to the host over binder.

"Linux-compatible" means the kext implements the **Linux binder ABI on the
wire**: the ioctl numbers, the command streams (`BC_*`/`BR_*`), and the struct
layouts are the ones the real driver uses, so an unmodified Android
`libbinder` works against it. The guest sees a normal `/dev/binder`; the host
kext underneath is our own.

Target scope (agreed): the complete core — `/dev/binder`, `/dev/hwbinder`,
`/dev/vndbinder`, plus binderfs and `/dev/ashmem`. Device devices first;
binderfs and ashmem in later milestones.

### Non-goals (this phase)

- Running a *full* Android userspace to completion. The integration target is
  "the binder plumbing works end to end under NABI", exercised by a test client,
  not "Waydroid boots a container".
- General Linux device compatibility. Only the devices listed above.
- DriverKit migration (there is no `cdevsw` in DriverKit; no forward path exists).

---

## 2. The three hard facts this design has to live with

These were verified against the SDK, the local XNU source
(`/Users/sunneva/Development/SDKs/Kernel/xnu`, tag `xnu-12377.1.9`, same release
train as the running `xnu-12377.121.10`), the KDK (`KDK_26.5.2_25F84.kdk`), and
the `config/*.exports` KPI export lists. Full citations are in the exploration
reports (see §10).

1. **The character-device control path is fully available.** `cdevsw_add`,
   `devfs_make_node`, `msleep`/`wakeup` (+`PCATCH`), `copyin`/`copyout`,
   `d_select`/`selrecord`/`selwakeup` are all in KPI symbol sets
   (`BSDKernel.exports`, `Libkern.exports`, `Mach.exports`). Nothing blocks the
   `/dev/binder` node itself.

2. **`mmap(2)` on a character device does not exist on macOS.** `kern_mman.c`
   returns `ENODEV` for any `VCHR` vnode; `cdevsw.d_mmap` is dead code that
   nothing calls. Binder's *receive buffer* is fundamentally an mmap'd region,
   so the data plane cannot ride the cdev. It must be served through an IOKit
   user client instead (see §4, "data plane").

3. **No kext can install a file descriptor into another process.** `falloc`,
   `fp_lookup`, `fp_drop` are not in any KPI export list. `BINDER_TYPE_FD` /
   `BINDER_TYPE_FDA` — passing a descriptor from one binder process to another —
   cannot be implemented inside the kext. It must be brokered in userspace
   (§4, "capability plane").

Both blockers are architectural, not incidental. The design below acknowledges
them instead of fighting them.

---

## 3. Architecture

Three planes, each owned by the layer that macOS actually allows to do it:

```
┌────────────────────────────  guest (Android libbinder under NABI)  ────────────────────────────┐
│   open() / ioctl() / poll() / mmap() / write()  all ordinary Linux syscalls on /dev/binder     │
└───────────────┬───────────────────────────────────────────┬───────────────────────────────────┘
                │ NABI translates                           │ NABI owns guest→host address space
                │                                            │
                ▼                                            ▼
   ┌──────────────────────────┐             ┌───────────────────────────────┐
   │  CONTROL PLANE (kext)    │             │  DATA PLANE (kext, C++)       │
   │  cdevsw + devfs_make_node│             │  IOService + IOUserClient     │
   │  /dev/binder, /dev/      │◄────────────►  shared receive buffer:       │
   │  hwbinder, /dev/vndbinder│  BINDER_WRITE_READ,  the kext writes BR_*  │
   │  open/close/ioctl/poll   │  BR_TRANSACTION,     into the buffer the    │
   │  binder core: proc/thread│  death, wakeup        guest mapped          │
   │  node/ref/transaction    │                         (single-copy)       │
   └──────────────┬───────────┘             └───────────────┬───────────────┘
                  │  opaque fd-token in the wire stream     │
                  ▼                                          │
   ┌──────────────────────────────────────────────────────────┘
   │  CAPABILITY PLANE (userspace)  — binder shim + binderd daemon
   │  BINDER_TYPE_FD/FDA: kext hands out opaque handles;
   │  the shim exchanges the real descriptors via SCM_RIGHTS
   └─────────────────────────────────────────────────────────
```

### Control plane — the cdev

- One `struct cdevsw` instance (initialised from `NO_CDEVICE`, `d_type = 0`),
  registered with `cdevsw_add(-1, &csw)` for auto-major allocation.
- One `devfs_make_node` per device with perms `0666`, root:wheel — matching
  Linux's `/dev/binder` — on paths `/dev/binder`, `/dev/hwbinder`,
  `/dev/vndbinder` (+ binderfs + ashmem in later milestones).
- `d_open` allocates the per-process state (`struct binder_proc`); `d_close`
  tears it down, wakes every waiter with the "peer died" sweep, and walks all
  remaining transactions free.
- `d_ioctl` is the Linux binder ioctl switch. The command *numbers* and struct
  layouts are Linux's, verbatim — no translation layer, because NABI passes the
  guest's Linux ioctl number through unchanged. `cmd` arriving as `int` in NABI
  and `u_long` in the kext is a non-issue: the bit patterns are identical.
- `d_select` + `selrecord`/`selwakeup` implement readiness; binder's userspace
  `poll(2)` on the driver fd rides this. **Requires vendoring the real
  `struct selinfo`** (the SDK only forward-declares it — see §7, "vendored XNU
  headers").
- Blocking: the `msleep(…, PCATCH, "…", &ts)` + `wakeup()` slot pattern copied
  from `mSL-ProcFS/kext/procfs_ctl.c`. `PCATCH` gives the interruptible wait the
  binder read loop needs.

### Data plane — the receive buffer

Binder's mmap gives each process a kernel-owned receive buffer: the driver
writes `BR_TRANSACTION`/`BR_REPLY` into it and the process reads them straight
out of the mapping. On macOS the only supported way to hand a kernel buffer to a
process as a mapping is IOKit: `IOBufferMemoryDescriptor::inTaskWithOptions` +
`IOUserClient::clientMemoryForType` (or `IOMemoryDescriptor::createMappingInTask`).

So the kext also publishes a companion `IOService`/`IOUserClient` (per the
`directhw` precedent for hand-publishing a service from a live-loaded kext).
The user client and the cdev share the per-process state: the cdev's softc *is*
where the receive buffer lives.

For a **native** client this would mean `open /dev/binder` + `IOServiceOpen`,
two steps. For **NABI**, the mapping can be transparent: NABI's `do_mmap` hook
(see §5) recognises a binder fd, calls `IOConnectMapMemory` to obtain the host
mapping, then `vmm_mmap`s those same pages into the guest via stage-2. The
guest's `mmap(NULL, BINDER_VM_SIZE, PROT_READ, MAP_PRIVATE, fd, 0)` then works
unchanged and single-copy. `vmm_mmap` requires a 16 KiB-aligned host pointer;
`IOBufferMemoryDescriptor` mappings satisfy that (document it).

### Capability plane — fd passing, in userspace

`BINDER_TYPE_FD`/`BINDER_TYPE_FDA` are the ABI's capability-passing objects.
The kext cannot move a descriptor across processes, so:

- The kext recognises FD objects in the wire stream, allocates an opaque token,
  and carries the token in place of the descriptor. The transaction proceeds.
- A userspace shim (a thin interpose layer around `libbinder`, or a `binderd`
  daemon in the mSL house style) performs the actual transfer out of band —
  over a `SCM_RIGHTS` unix socket, which NABI already translates
  (`src/net/net.c`, SCM_RIGHTS is implemented). The kext only brokers the
  rendezvous: it tells the shim "process A is passing a descriptor to process
  B"; the shim moves it and hands both sides back their token ↔ fd mapping.

This is the one place the guest's `libbinder` cannot stay stock: Android's
`libbinder` does fd passing inline in the transaction. The shim intercepts at
the `BINDER_TYPE_FD` boundary, which is exactly where the ABI's fd objects
appear in the parcel stream. Design the shim boundary in M4; do not let it leak
into M1–M3.

---

## 4. Repository layout (house conventions)

Replicate `mSL-SysFS` **minus `fs/`** (no VFS type to register → no `.fs` mount
bundle). That lands on `directhw`'s shape with SysFS's discipline.

```
mSL-DevFS/
├── .github/workflows/c-cpp.yml   CI: make ARCH=arm64e / x86_64 / universal
├── .gitignore                    copy SysFS's verbatim
├── .gitmodules                   lib/libkext, lib/xnu, lib/MacKernelSDK (+lib/libsbuf if needed)
├── LICENSE                       MIT, "Copyright (c) 2026 Sunneva N. Mariu"
├── Makefile                      all-in-one orchestrator (build/install/unload/clean)
├── Makefile.inc                  KEXTNAME=devfs, BUNDLEDOMAIN=com.beako.filesystems, VERSION…
├── README.md                     mSL/SysFS template; extend the mSL/XNU module table
├── VERSION                       0.0.1
├── include/fs/devfs/*.h          shared: bundle defs, binder wire structs, ctl protocol
├── include/xnu/                  vendored XNU private headers (+ bsd/sys/conf.h)
├── kext/                         the kernel extension (C + one C++ TU)
├── lib/                          git submodules
├── out/                          build output (gitignored)
└── tools/                        userspace smoke test, (M4) binderd
```

`Makefile.inc` specifics:

- `KEXTNAME?= devfs`, `KEXTVERSION` read from `VERSION`, `SIGNCERT?= -` with the
  keychain-verification guard, `COPYRIGHT` as in siblings.
- `BUNDLEDOMAIN?= com.beako.filesystems` → bundle id
  `com.beako.filesystems.devfs`, lock group
  `com.beako.filesystems.devfs.lckgrp`.
- **Name-collision warning (document in Makefile.inc):** XNU already ships a
  `devfs` VFS. The bundle id and lock group are unique, but the repo's own
  `make stat`/`make unload` targets that grep `kextstat` must grep for the
  bundle id (`com.beako.filesystems.devfs`), not the bare name.
- `FSNAME`/`FSVERSION`/`FSBUILD` are dropped (no filesystem bundle).

`kext/Makefile` carries over verbatim from `mSL-SysFS/kext/Makefile`:

- The kernel-private `-D` set (`KERNEL`, `PRIVATE`, `XNU_KERNEL_PRIVATE`,
  `BSD_KERNEL_PRIVATE`, `MACH_BSD`, `XNU_PLATFORM_MacOSX`, …).
- **`-DCONFIG_PERSONAS -DCONFIG_AUDIT -DCONFIG_DTRACE`** with the SysFS-style
  rationale comment. This kext dereferences `struct proc`/`struct task` more
  than sysfs does (per-proc creds in transactions, `sender_euid` checks), so the
  RELEASE-kernel struct-layout contract is load-bearing.
- `-DAPPLEFIRESTORM` for arm64.
- The one C++ TU (`devfs_mmap.cpp`) drops `$(XNU_INCLUDES)` per the
  `sysfs_iokit.cpp` precedent.
- Link `-lkmod -lkext` (libkext's `STATIC`, logging, `kassert` helpers).

### Vendored XNU headers

Copy `mSL-SysFS/include/xnu` and **add `bsd/sys/conf.h`** (the `struct cdevsw` /
`cdevsw_add` declarations), which neither sibling tree currently has. Source:
`mSL-ProcFS/lib/xnu/bsd/sys/conf.h`. Document the addition in the kext Makefile
comment style (why it is needed, which sibling lacks it, what version it came
from).

`bsd/sys/select.h` is already in both vendored trees — that is what lets us
embed `struct selinfo` by value instead of over-allocating an opaque byte array.
Note in a comment that this is an explicit ABI gamble across OS updates (the
struct size differs between Mach and BSD views — that is why Apple made it
opaque), and that it is the same gamble the sibling projects already take with
their other vendored structs.

### Naming / style (from the house-conventions report)

- Public functions `<fs>_<verb>` (`devfs_open`, `binder_ioctl`, `binder_thread_…`).
- File-static globals `g_`-prefixed; file-local statics use `STATIC`, not
  `static`.
- Guard macros `_FS_DEVFS_<FILE>_H_`; wire magics as hex-`u` with `/* '…' */`.
- Logging: `printf("devfs: …\n")` for subsystem messages, `os_log` in
  start/stop only, `LOG_ERR`/`kassert` reserved for "this shouldn't happen".
- File headers: the multi-paragraph "what / why / what was rejected" prose,
  `Copyright (c) 2026 Sunneva N. Mariu`.
- Memory: `OSMalloc`/`OSFree` with one `OSMallocTag` from the bundle id in C;
  `IOMallocZeroData` for byte buffers; `IOMalloc` wrappers in the C++ TU only.
  `_MALLOC` never.

---

## 5. Milestones

Each milestone ends with something loadable/testable. Nothing is "implemented"
until it has been run.

### M0 — Scaffold + proof-of-life kext

**Deliverables:** the §4 skeleton, minus binder logic. A stub kext that does
`cdevsw_add` + `devfs_make_node("/dev/binder", …)` with an `eno_*`-style
`d_open`/`d_close`/`d_ioctl` that answers `BINDER_VERSION` (protocol 8),
loads, and unloads cleanly.

**Do now:**

- `git submodule update --init` for `lib/libkext` (+ `lib/xnu`, `lib/MacKernelSDK`).
- Add `include/xnu/bsd/sys/conf.h`; make sure the `make ARCH=arm64e` build is
  green on the compile gate **before** writing any binder logic.
- `make ARCH=arm64e` for all three arches + `universal` in CI (copy the SysFS
  workflow), confirming the kext compiles under `-Wall -Wextra` with the vendored
  private headers in kernel-private configuration.
- Live-load smoke test (this machine already runs the sibling kexts under the
  same reduced-security posture): `kmutil load -p`, `kmutil showloaded`, check
  `/dev/binder` exists and `BINDER_VERSION` answers 8 from a tiny `tools/`
  probe, then `kmutil unload`. **Add the `kmutil clear-staging` pre-step to the
  Makefile unload/reload targets** (the stale-AuxKC-shadow pitfall documented in
  mSL-ProcFS).

**Acceptance:** `/dev/binder` exists on a live system; `BINDER_VERSION` returns
8; kext unloads; `make ARCH={arm64e,x86_64}` + `make ARCH=universal` pass in CI.

### M1 — Protocol core (cdev only, copy-based)

The binder driver core: per-process/per-thread state, the node/ref model, the
transaction engine, blocking read, readiness. No mmap, no fd passing — the read
path uses `copyout` at ioctl completion.

**kext files:**

- `kext/binder.c` — `binder_proc` (per open fd), `binder_thread` (per thread
  that entered the loop), `binder_node` (registered objects / context manager),
  `binder_ref` (handles), `binder_transaction`, the `todo`/`delivered` queues,
  death notifications. Locking: `lck_rw_t` for the global proc list,
  `lck_mtx_t` per proc, per the `lck_*` KPI (all exported).
- `kext/devfs_cdev.c` — the `struct cdevsw` handlers; `d_ioctl` switch on the
  Linux ioctl numbers (below).
- `kext/devfs_alloc.c` — per-proc receive buffer (kmalloc'd this milestone;
  becomes the IOKit descriptor in M2).
- `kext/devfs_ctl.c` — diagnostic sysctls: in-flight transactions, allocated
  buffers, live procs/nodes/refs, per the hand-built `struct sysctl_oid` pattern
  in `sysfs.c`.

**Ioctls** (Linux numbers, literal hex; `/* _IOx('b', n, type) */` comments):
`BINDER_WRITE_READ` 0xC0306201, `BINDER_SET_MAX_THREADS` 0x40046205,
`BINDER_SET_CONTEXT_MGR` 0x40046207, `BINDER_SET_CONTEXT_MGR_EXT` 0x4014620B,
`BINDER_THREAD_EXIT` 0x40046208, `BINDER_VERSION` 0xC0186209,
`BINDER_SET_IDLE_TIMEOUT` 0x40086203, `BINDER_SET_IDLE_PRIORITY` 0x40046206.
The command **streams** (`BC_TRANSACTION_SG`/`BC_REPLY_SG` + scatter-gather
objects, `BC_FREE_BUFFER`, `BC_*` refcount/death commands, `BR_*` returns) are
implemented inside `BINDER_WRITE_READ` per the modern driver.

**NABI-side (this milestone):**

- `include/linux/binder.h` — converted from a **current** Linux
  `include/uapi/linux/android/binder.h` into the `LINUX_`/`l_*` house style.
  ⚠️ Do **not** lift from
  `test/testing_root/usr/include/linux/android/binder.h`: it is Linux **4.7**-era
  (pre-scatter-gather, no FDA), useless for Waydroid. Protocol target: version 8,
  scatter-gather + `flat_binder_object` security ctx. This header and the kext's
  wire structs are a **cross-repo contract** — the two must be size-identical;
  state it in both headers' comments.
- `binder_ioctl(struct file *, int cmd, uint64_t val0)` called from the top of
  `darwinfs_ioctl` in `src/fs/fs.c` — the `eventfd_lookup` precedent
  (`fs.c:516,525`): a side table (khash `int fd → struct binder_state`), a
  `binder_close(fd)` hook in `user_close`, state populated at open by recognising
  the node (the `F_GETPATH`/`fstat` path already available at `fs.c:3388`).
- Guest-pointer translation: `copy_from_user` the `binder_write_read`, translate
  `write_buffer`/`read_buffer` (and every nested `data.ptr.buffer`/`.offsets`
  inside the command stream) through `guest_to_host`, re-copy on the way back —
  the `sendmsg` cmsg model (`net.c:556-700`). Never hand a host pointer to the
  guest.
- Wrap the blocking `BINDER_WRITE_READ` in `RETRY_ON_RESTARTABLE_EINTR` so a
  NABI-internal signal does not surface to the guest as a spurious EINTR.
- `Makefile`: add the new source to `COMMON_SRCS`; add `-framework IOKit` only
  when M2 lands.

**Acceptance:** a Linux test binary under NABI can `open("/dev/binder")`,
`BINDER_VERSION`, register as context manager, and perform a **self-transaction**
(register a node, acquire a handle, `BC_TRANSACTION` → `BR_TRANSACTION` round
trip through the read loop, `BC_FREE_BUFFER`) — on one process. Copy-based read
path is fine for this milestone.

### M2 — Data plane: shared receive buffer (the mmap)

**kext:**

- `kext/devfs_mmap.cpp` — `IOService` + `IOUserClient`, hand-published from the
  kmod start per `directhw/kext/DirectHW_kmod.c`. `clientMemoryForType` returns
  an `IOBufferMemoryDescriptor::inTaskWithOptions` for the binder proc's receive
  buffer; the cdev softc and the user client share it.
- The read path switches from `copyout` to writing `BR_*` directly into the
  mapped buffer; `BINDER_WRITE_READ`'s `read_buffer`/`consumed` bookkeeping is
  done against the mapping.
- Unmap/free on close, mapped region accounting in the diagnostics sysctls.

**NABI:**

- `do_mmap` device hook (`src/mm/mmap.c`): a binder fd mapping — even
  `MAP_PRIVATE` — is a real host mapping, not an arena copy. Recognise the fd,
  `IOConnectMapMemory` on the user-client connection to obtain the host address,
  then `vmm_mmap` it into the guest (same pages, no copy). Keep the existing
  16 KiB alignment assert; document that the returned mapping satisfies it.
- The `pread` fallback currently in `do_mmap` must **not** run for binder
  (it would return an errno from a char device that implements no read).

**Acceptance:** guest mmap of `/dev/binder` succeeds; an async
`BR_TRANSACTION` written by the kext (from another thread/proc) appears in the
guest's mapped buffer and is read without `copyout`. Two-process transaction
(still no fd passing) round-trips. This is the milestone that makes the design
real.

### M3 — Full device set, binderfs, ashmem

- `devfs_make_node` for `/dev/hwbinder`, `/dev/vndbinder`, `/dev/ashmem`.
- **binderfs:** on Linux this is a small fs, mounted by Waydroid at a path of
  its choosing, that auto-populates `binder`/`hwbinder`/`vndbinder`. Feasibility
  to verify first (the M0 work on `devfs_make_node` pathname handling): can
  `devfs_make_node` create a node under a `binderfs/` subpath of `/dev`? If yes,
  publish `/dev/binderfs/{binder,hwbinder,vndbinder}` and have NABI's
  `backing_for_type` map `binderfs` to "already served, no host dir" (the
  existing `mount.c` precedent for `devtmpfs`/`devpts`). If not, a real VFS is a
  separate follow-up — call it out, don't bundle it here.
- **ashmem:** `/dev/ashmem` → kext-backed anonymous shared memory (fd passing
  between processes over binder is what makes ashmem useful; note the M4
  dependency). Modern Android prefers memfd (NABI has memfd, unsealed); document
  what ashmem covers that memfd does not.

**Acceptance:** all four nodes exist; binderfs path resolves under NABI; ashmem
mmap/fd round-trip works between two processes (its own test, independent of
binder).

### M4 — Capability plane: BINDER_TYPE_FD/FDA

- Kext: FD objects in the command stream become opaque tokens; token↔(proc,fd)
  bookkeeping; `BR_` errors when the shim is absent.
- Userspace: the shim (interpose layer or `com.beako.binderd` daemon) performs
  the real transfer over `SCM_RIGHTS` — already implemented in NABI's
  `src/net/net.c` — keyed by the tokens the kext passes through.
- This is the only place guest `libbinder` needs help; scope the interpose
  surface precisely and keep it isolated.

**Acceptance:** a two-process test passes an fd (a pipe or ashmem region)
through a binder transaction and reads it on the far side.

---

## 6. NABI-side work — summary table

| Change | Location | Milestone |
|---|---|---|
| `include/linux/binder.h` (fresh uapi → house style) | `mSL-NABI/include/linux/binder.h` | M1 |
| `binder_ioctl` hook + fd side table + `binder_close` | `mSL-NABI/src/fs/fs.c` | M1 |
| Guest-pointer translation for binder ioctls | `mSL-NABI/src/fs/fs.c` | M1 |
| `RETRY_ON_RESTARTABLE_EINTR` on blocking read | `mSL-NABI/src/fs/fs.c` | M1 |
| `do_mmap` device-aware hook + `IOConnectMapMemory` | `mSL-NABI/src/mm/mmap.c` (+ `-framework IOKit`) | M2 |
| `backing_for_type` binderfs case | `mSL-NABI/src/fs/mount.c` | M3 |
| `mknod` for char devices (LXC populates `/dev`) | `mSL-NABI/src/fs/fs.c:4293` | M3 (stretch) |
| SCM_RIGHTS rendezvous for fd passing | shim / `binderd` | M4 |

Cross-repo contract: `mSL-NABI/include/linux/binder.h` and
`mSL-DevFS/include/fs/devfs/binder.h` must be byte-identical on the wire
structs. Both headers carry a comment naming the other.

---

## 7. Open questions / risks

1. **`devfs_make_node` with a pathname containing `/`** — needed for binderfs
   under `/dev/binderfs/…`. XNU's `devfs_make_node_internal`/`dev_add_entry`
   pathname handling must be read and tested early (M0). If nested paths are
   unsupported, binderfs moves to a real VFS and its estimate grows.
2. **`struct selinfo` vendoring** is an ABI gamble (size differs between Mach
   and BSD views). Accepted — same class of gamble the siblings already take —
   but it must be a named, commented decision.
3. **IOUserClient ↔ cdev sharing.** The user client's `clientMemoryForType`
   produces a mapping for the *calling task*. For NABI that is exactly right
   (NABI is the host process); for a hypothetical native client it means two
   opens. Document the assumption; do not build for the native case yet.
4. **Protocol version drift.** The kext implements a moving target. Pin the
   protocol to the header we convert in M1 (v8 + scatter-gather) and say so in
   the README.
5. **`kmutil clear-staging`** on every reload — add to the Makefile now, not
   when a stale AuxKC hides a working build.
6. **Waydroid is still not just binder.** Even at M4 completion, LXC container
   bring-up needs real `mknod`, `clone(CLONE_NEWNS|NEWPID|NEWNET)`, cgroup
   controllers, sealing — per the NABI report §6. State loudly that this
   project removes the *binder* wall, not the whole container stack.

---

## 8. Testing strategy

- **Kext:** no in-kernel test framework; integration tests against a live load,
  driven from `make check` (skip cleanly in CI like ProcFS's). `tools/binder-test.c`
  exercises version, context-manager registration, self-transaction, and (M2)
  mapped-buffer async delivery; `tools/ashmem-test.c` in M3.
- **NABI integration:** run the Linux test binaries under `nabi` (the existing
  `tests/test_features.sh` harness shape) so the *translated* path is what is
  verified, not just the kext.
- **CI:** compile gate only — `make ARCH=arm64e`, `x86_64`, `universal` — exactly
  the SysFS/ProcFS workflow. No signing, no hardware, no `make check` on CI.
- **Performance sanity (M2):** a transaction round-trip over the mapped buffer
  vs the M1 copy path, to prove the single-copy claim isn't theory.

---

## 9. Order of work

1. M0 scaffold + proof-of-life (this is the first PR-ready state).
2. M1 protocol core + NABI `binder.h`/ioctl hook. Self-transaction passing is the
   milestone gate.
3. M2 data plane; `do_mmap` hook; two-process transaction.
4. M3 devices + binderfs + ashmem.
5. M4 fd passing via the shim.

Each milestone is independent enough to merge, load, and demo alone. Do not
design M4's shim until M2's buffer model is proven — the fd-token design depends
on the wire-stream shape, which the self-transaction test fixes first.

---

## 10. Sources

This plan is built on three exploration reports produced for the earlier session
(raw transcripts retained in `/private/tmp/claude-501/…/tasks/*.output`):

1. **XNU KPI feasibility** — SDK headers, local XNU source, KDK, and
   `config/*.exports` verification of every symbol the design touches
   (cdevsw/devfs, mmap `ENODEV` proof, `BINDER_TYPE_FD` blocker, lock/select/
   copyin availability, kext-loading posture on this machine).
2. **NABI integration** — path passthrough for `/dev`, the `file_operations`/
   side-table model, guest↔host address translation, `do_mmap` arm64 behavior,
   the vendored 4.7-era binder header, and the remaining container gaps.
3. **House conventions** — the exact skeleton, `Makefile.inc`, vendored-header
   contract, `-D` configuration, logging/locking/alloc idioms, ctl slot-table
   blocking pattern, and installer/CI conventions from the sibling projects.
