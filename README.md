# mSL/DevFS

`/dev/binder` (and friends) for Android/Waydroid userspace running under mSL/NABI.

## Status

**M0 complete** — scaffold + proof-of-life kext implemented. The kext compiles for arm64e, x86_64, and universal (fat) architectures, registers a cdevsw, creates `/dev/binder`, and answers `BINDER_VERSION` ioctl with protocol version 8.

## Architecture

Three planes, each owned by the layer that macOS actually allows:

- **Control plane (kext)** — `cdevsw` + `devfs_make_node` for `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder`. `d_open`/`d_close`/`d_ioctl`/`d_select` with `msleep`/`wakeup` blocking.
- **Data plane (kext, C++)** — `IOService` + `IOUserClient` for the shared receive buffer (the `mmap` that Linux binder requires).
- **Capability plane (userspace)** — `BINDER_TYPE_FD`/`FDA` fd passing brokered via `SCM_RIGHTS` over a userspace shim/daemon (M4).

## Repository Layout

```
mSL-DevFS/
├── .github/workflows/c-cpp.yml   CI: make ARCH=arm64e / x86_64 / universal
├── .gitignore
├── .gitmodules                   lib/libkext, lib/xnu, lib/MacKernelSDK
├── LICENSE                       MIT
├── Makefile                      all-in-one orchestrator (build/install/unload/clean)
├── Makefile.inc                  KEXTNAME=devfs, BUNDLEDOMAIN=com.beako.filesystems
├── VERSION                       0.0.1
├── README.md                     this file
├── include/fs/devfs/
│   ├── binder.h                  shared wire protocol (ioctls, binder_version)
│   └── devfs.h                   kernel-only bundle defs, cdevsw
├── include/xnu/                  vendored XNU private headers (+ bsd/sys/conf.h)
├── kext/                         the kernel extension
├── lib/                          git submodules
├── out/                          build output (gitignored)
└── tools/                        userspace smoke test (binder-probe)
```

## Building

```bash
# Initialize submodules
git submodule update --init

# Build for native architecture (arm64e on Apple Silicon)
make

# Build for specific architecture
make ARCH=arm64e
make ARCH=x86_64

# Build universal (fat) kext
make ARCH=universal
```

Artifacts are placed in `out/`:
- `devfs.kext` — the kernel extension bundle
- `binder-probe` — userspace smoke test

## Installing / Loading

```bash
# Install to /Library/Extensions (requires root)
sudo make install

# Load the installed kext (requires root)
sudo make load

# Unload
sudo make unload

# Uninstall
sudo make uninstall
```

## Testing

After loading, run the smoke test:

```bash
out/binder-probe
# Expected output:
# BINDER_VERSION: protocol_version=8
# PASS: BINDER_VERSION returns 8
```

## Milestones

- **M0** (complete) — Scaffold + proof-of-life: `/dev/binder` exists, `BINDER_VERSION` returns 8
- **M1** — Protocol core (cdev only, copy-based): per-process/thread state, node/ref model, transaction engine, blocking read
- **M2** — Data plane: shared receive buffer via `IOUserClient`, `mmap` support via NABI `do_mmap` hook
- **M3** — Full device set: `/dev/hwbinder`, `/dev/vndbinder`, `/dev/ashmem`, binderfs
- **M4** — Capability plane: `BINDER_TYPE_FD`/`FDA` via userspace `SCM_RIGHTS` shim

## Cross-Repo Contract

`mSL-DevFS/include/fs/devfs/binder.h` and `mSL-NABI/include/linux/binder.h` (M1) must be byte-identical on wire structs and ioctl values.

## License

MIT License — Copyright (c) 2026 Sunneva N. Mariu