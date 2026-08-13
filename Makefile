#
# All-in-one Makefile - mSL/DevFS
#
# Usage:
#   make                    # build libs + kext into $(OUT)
#   make ARCH=arm64e        # Apple Silicon kext (default on arm64)
#   make ARCH=x86_64        # Intel kext
#   make ARCH=universal     # fat kext (arm64e + x86_64)
#   sudo make install       # install the built artifacts (run AFTER make)
#   sudo make uninstall     # remove them
#   make clean              # remove build artifacts (no sudo needed)
#
# mSL/DevFS has no VFS type and no mount bundle, so unlike the mSL/SysFS and
# mSL/ProcFS siblings there is no fs/ subtree and no mount_sysfs - the build
# produces just the kext (plus its supporting libraries) and the userspace
# smoke-test binaries in tools/.
#
# NOTE: never run the build as root. `make install` only COPIES the already-built
# artifacts from $(OUT) into place; it does not compile. This keeps every build
# artifact owned by the invoking user, so `make clean` never needs sudo.
#

MAKE=make
OUT=out

# Install location. The kext is a plain /Library/Extensions drop-in like the
# siblings; there is no filesystem bundle to place in /Library/Filesystems.
EXT_DIR        := /Library/Extensions
BUNDLE_ID      := $(shell sed -n 's/^BUNDLEDOMAIN?=\t*//p' Makefile.inc | head -1).$(shell sed -n 's/^KEXTNAME?=\t*//p' Makefile.inc | head -1)

# Detect native arch if ARCH not specified
NATIVE_ARCH := $(shell uname -m)
ifeq ($(NATIVE_ARCH),arm64)
    DEFAULT_ARCH := arm64e
else
    DEFAULT_ARCH := x86_64
endif
ARCH ?= $(DEFAULT_ARCH)
# Accept arm64 as alias for arm64e (kexts require arm64e ABI)
ifeq ($(ARCH),arm64)
    override ARCH := arm64e
endif

# Per-arch settings
ifeq ($(ARCH),arm64e)
    KEXT_ARCHFLAGS    := -arch arm64e
    KEXT_TRIPLE       := arm64e-apple-macos12.0
    LIB_ARCHFLAGS     := -arch arm64e
    LIB_TRIPLE        := arm64e-apple-macos12.0
else ifeq ($(ARCH),x86_64)
    KEXT_ARCHFLAGS    := -arch x86_64
    KEXT_TRIPLE       := x86_64-apple-macos10.15
    LIB_ARCHFLAGS     := -arch x86_64
    LIB_TRIPLE        := x86_64-apple-macos10.15
else ifeq ($(ARCH),universal)
    # The universal kext target builds each arch explicitly and lipos them;
    # these defaults just need to be valid (the arm64e slice).
    KEXT_ARCHFLAGS    := -arch arm64e
    KEXT_TRIPLE       := arm64e-apple-macos12.0
    LIB_ARCHFLAGS     := -arch arm64e
    LIB_TRIPLE        := arm64e-apple-macos12.0
else
    $(error Unknown ARCH=$(ARCH). Use arm64e, x86_64, or universal)
endif

KEXT_FLAGS := ARCHFLAGS="$(KEXT_ARCHFLAGS)" TARGET_TRIPLE="$(KEXT_TRIPLE)"
LIB_FLAGS  := ARCHFLAGS="$(LIB_ARCHFLAGS)"  TARGET_TRIPLE="$(LIB_TRIPLE)"

# The build wipes and repopulates $(OUT) in a fixed order; never parallelise it.
.NOTPARALLEL:

# ---------------------------------------------------------------------------
# Build  ->  $(OUT)
# ---------------------------------------------------------------------------

# `clean` first so a build always starts from a clean tree. This matters when
# several arches are built in one checkout (e.g. CI runs `make ARCH=arm64e` then
# `make ARCH=x86_64`): the single-arch build only wipes $(OUT), so without this
# the second build would relink the first arch's stale .o/.a files and fail with
# "found architecture 'arm64e.kernel', required architecture 'x86_64'".
all: clean kextfs

ifeq ($(ARCH),universal)

# kext as a fat (arm64e + x86_64) binary: build each arch, then lipo the
# Mach-O together and re-sign the bundle.
kextfs:
	rm -rf $(OUT)
	mkdir $(OUT)
	$(MAKE) -C lib  ARCHFLAGS="-arch arm64e" TARGET_TRIPLE="arm64e-apple-macos12.0"
	$(MAKE) debug -C kext ARCHFLAGS="-arch arm64e" TARGET_TRIPLE="arm64e-apple-macos12.0"
	mv kext/devfs.kext kext/devfs.kext.dSYM $(OUT)
	mv $(OUT)/devfs.kext $(OUT)/devfs.kext.arm64e
	$(MAKE) -C kext clean
	$(MAKE) -C lib clean
	$(MAKE) -C lib  ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	$(MAKE) debug -C kext ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	rm -rf $(OUT)/devfs.kext.dSYM
	mv kext/devfs.kext kext/devfs.kext.dSYM $(OUT)
	mv $(OUT)/devfs.kext $(OUT)/devfs.kext.x86_64
	cp -r $(OUT)/devfs.kext.arm64e $(OUT)/devfs.kext
	lipo -create $(OUT)/devfs.kext.arm64e/Contents/MacOS/devfs $(OUT)/devfs.kext.x86_64/Contents/MacOS/devfs -output $(OUT)/devfs.kext/Contents/MacOS/devfs
	codesign --force --timestamp=none --sign - $(OUT)/devfs.kext
	rm -rf $(OUT)/devfs.kext.arm64e $(OUT)/devfs.kext.x86_64
	$(MAKE) -C tools
	mv tools/binder-probe $(OUT)/
	mv tools/binder-abi-test $(OUT)/
	-mv tools/binder-probe.dSYM $(OUT)/ 2>/dev/null || true
	-mv tools/binder-abi-test.dSYM $(OUT)/ 2>/dev/null || true

else

# kext + tools for a single arch.
kextfs:
	rm -rf $(OUT)
	mkdir $(OUT)
	$(MAKE) -C lib  $(LIB_FLAGS)
	$(MAKE) debug -C kext $(KEXT_FLAGS)
	mv kext/devfs.kext kext/devfs.kext.dSYM $(OUT)
	$(MAKE) -C tools
	mv tools/binder-probe $(OUT)/
	mv tools/binder-abi-test $(OUT)/
	-mv tools/binder-probe.dSYM $(OUT)/ 2>/dev/null || true
	-mv tools/binder-abi-test.dSYM $(OUT)/ 2>/dev/null || true

endif

# ---------------------------------------------------------------------------
# Load / unload  (operate on the built $(OUT); need root)
# ---------------------------------------------------------------------------

# kmutil clear-staging first: macOS caches third-party kexts in the Auxiliary
# Kernel Collection, and a stale staged copy otherwise shadows a freshly built
# kext - the "new" load silently succeeds against an old binary (documented
# pitfall in mSL-ProcFS). Always clear staging before a load, not just after a
# prior install.
load: kextfs
	sudo kmutil clear-staging 2>/dev/null || true
	sudo cp -R $(OUT)/devfs.kext /tmp/devfs.kext
	sudo chown -R root:wheel /tmp/devfs.kext
	sudo chmod -R 755 /tmp/devfs.kext
	sudo kmutil load -p /tmp/devfs.kext
	sudo kmutil showloaded | grep devfs || true
	sudo rm -rf /tmp/devfs.kext

# Errors are reported rather than swallowed: an unload that quietly fails
# leaves the old build resident, and the `load` below then succeeds against
# it without replacing anything, so a fix appears not to work.
unload:
	-sudo kmutil unload -b $(BUNDLE_ID)
	sudo kmutil clear-staging 2>/dev/null || true

# Unload, load, and PROVE the running kext is the one just built. kmutil load
# silently succeeds when a bundle of the same id and version is already
# resident, so "did it actually reload?" is not a question worth answering by
# eye - it costs a debugging session every time the answer is no.
reload: kextfs
	-sudo kmutil unload -b $(BUNDLE_ID)
	@if kmutil showloaded 2>/dev/null | grep -q '$(BUNDLE_ID)'; then \
		echo "error: $(BUNDLE_ID) is still loaded - the unload failed."; \
		echo "       Close anything holding /dev/binder and try again."; \
		exit 1; \
	fi
	@$(MAKE) --no-print-directory load
	@live=`kmutil showloaded 2>/dev/null | awk '/$(BUNDLE_ID)/ {print $$8}'`; \
	if [ -z "$$live" ]; then \
		echo "error: $(BUNDLE_ID) did not load."; exit 1; \
	elif dwarfdump --uuid $(OUT)/devfs.kext/Contents/MacOS/devfs | grep -q "$$live"; then \
		echo "devfs: running $$live, which is this build."; \
	else \
		echo "error: running $$live, which is NOT this build - a stale copy is"; \
		echo "       still resident. Try: sudo kmutil clear-staging; reboot."; \
		exit 1; \
	fi

# The functional test, against a live load. Skips rather than fails when the
# kext is not loaded, so CI stays a compile gate (the same shape as the procfs
# sibling's `check`).
check: kextfs
	@if [ -e /dev/binder ]; then \
		echo "==> binder functional test"; \
		$(OUT)/binder-probe; rc=$$?; \
		if [ $$rc -eq 77 ]; then \
			echo "SKIP: nothing to test against"; exit 0; \
		fi; \
		exit $$rc; \
	else \
		echo "SKIP: /dev/binder is not present (sudo make load first)"; \
	fi

# ---------------------------------------------------------------------------
# Install / uninstall  (operate on the already-built $(OUT); need root)
# ---------------------------------------------------------------------------

install:
	@test -d "$(OUT)/devfs.kext" || { echo "error: build artifacts missing in $(OUT)/. Run 'make' first."; exit 1; }
	sudo make uninstall
	-sudo kmutil clear-staging 2>/dev/null || true
	sudo cp -r "$(OUT)/devfs.kext" "$(EXT_DIR)/devfs.kext"
	sudo chmod -R 755 "$(EXT_DIR)/devfs.kext"
	sudo chown -R root:wheel "$(EXT_DIR)/devfs.kext"
	@echo "devfs: installed the kext."
	@echo "devfs: auto-load stays DISABLED. To load it now:"
	@echo "         sudo make load"
	@echo "devfs: or load from the installed location:"
	@echo "         sudo kmutil load -p $(EXT_DIR)/devfs.kext"

uninstall:
	-sudo kmutil unload -b $(BUNDLE_ID) 2>/dev/null || true
	-sudo kmutil clear-staging 2>/dev/null || true
	-sudo rm -rf "$(EXT_DIR)/devfs.kext" || true
	@echo "devfs: unloaded and removed the kext."

clean:
	$(MAKE) -C lib clean || true
	$(MAKE) -C kext clean || true
	$(MAKE) -C tools clean || true
	rm -rf $(OUT)

.PHONY: all kextfs load unload reload check install uninstall clean
