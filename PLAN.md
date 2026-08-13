# mSL/DevFS — what is done, and what is next

This supersedes the earlier draft of this file, which planned an IOKit user
client for the data plane and described an ABI that turned out to be Linux
4.7-era with fabricated ioctl encodings. Both were replaced; the reasons are in
[README.md](README.md) and in the header comments of the files concerned.

## Done

The binder driver core, built and linking clean for arm64e, x86_64 and
universal, with every undefined symbol resolving against the KPI export lists:

- `kext/binder_alloc.c` — the client-registered transaction arena
- `kext/binder_node.c` — objects, handles, reference counting, death notification
- `kext/binder_txn.c` — transactions, replies, object translation, scatter-gather
- `kext/binder_proc.c` — processes, threads, queues, the read and write loops
- `kext/binder_dev.c` — the cloning devices, the three contexts, binderfs control
- `kext/devfs.c` — module bring-up and the `sysctl devfs` counters
- `include/fs/devfs/binder.h` — the ABI, with `tools/binder-abi-test.c` proving
  every constant against its `_IOC` derivation at compile time
- `tools/binder-probe.c` — the functional test
- `doc/NABI-INTEGRATION.md` — the NABI half, specified against real call sites

## Next, in order

1. **Run it.** `sudo make unload && sudo make load && make check`. The driver has
   never been loaded; an M0 scaffold from an earlier session was resident during
   development, which is why the probe now refuses to report on a driver that is
   not this build. Expect to spend the first pass here.

2. **Verify the devfs pathname assumption.** `/dev/binderfs/binder-control` is
   created with a `/` in the name, which XNU's `devfs_make_node` parses into
   directory components (`bsd/miscfs/devfs/devfs_tree.c`). If the node does not
   appear, fall back to a flat `/dev/binder-control` and say so in the README —
   the driver does not otherwise care.

3. **Land the NABI shim**, per `doc/NABI-INTEGRATION.md`. The order that gets a
   guest talking soonest: recognise the fd, intercept the ioctl, translate the
   two pointers in `binder_write_read`, provide the arena. Descriptor passing
   comes after a guest can transact at all.

4. **The descriptor broker.** Only once the shim works, because its shape
   depends on the wire traffic being real.

5. **Then the deferred pieces**, in whatever order the traffic demands them:
   `BINDER_TYPE_FDA`, nested scatter-gather, the security-context transaction
   form, freeze. Each is refused explicitly today rather than half-served.

## Deliberately not done

- **Fine-grained locking.** One mutex, with the reasoning written out in
  `include/fs/devfs/binder_internal.h`. The `sysctl devfs` counters are there to
  show when it starts to matter.
- **`/dev/ashmem`.** A character device cannot be mapped on macOS, and Darwin
  shared memory does the whole job in userspace. Specified in the NABI document.
- **Anything that makes Waydroid boot.** This removes the binder wall. The
  others — `mknod` for character devices, `clone` with namespace flags, backed
  `devtmpfs`, cgroup controllers, memfd sealing, per-thread signals — are listed
  with their locations at the end of `doc/NABI-INTEGRATION.md`.
