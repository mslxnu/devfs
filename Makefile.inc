#
# Put user-defined variables here
#
# mSL/DevFS: the Linux character devices (/dev/binder et al.) for Android
# userspace under mSL/NABI. Replicates the mSL/SysFS skeleton MINUS fs/ (there
# is no VFS type to register, so there is no .fs mount bundle), which lands on
# directhw's shape with SysFS's discipline. Unlike SysFS this tree keeps NO
# FSNAME/FSVERSION/FSBUILD variables: they exist only to feed a filesystem
# mount bundle, and DevFS is not one.
#

KEXTNAME?=				devfs
# Single source of truth: the repo VERSION file (next to this Makefile.inc).
# Bump the version by editing VERSION; the kext defines and Info.plist all pick
# it up from here. Overridable from the command line / environment
# (KEXTVERSION=...).
KEXTVERSION?=			$(strip $(shell cat $(dir $(lastword $(MAKEFILE_LIST)))VERSION 2>/dev/null || echo 0.0.1))
KEXTBUILD?=				1
BUNDLEDOMAIN?=			com.beako.filesystems
COPYRIGHT?= 			"Copyright © 2022 - 2026 Sunneva N. Mariu, All rights reserved."
#
# Code-signing identity. "-" means ad-hoc, which is what a development build
# wants: loading a kext with a real certificate needs a Developer ID with
# Apple's kext-signing entitlement, which an ordinary "Apple Development" cert
# is not.
#
# Overridable from the environment, so verify the identity actually exists
# before handing it to codesign. A stale SIGNCERT left exported in a shell -
# pointing at a certificate that has since been removed - would otherwise fail
# the build late, at the codesign step, with "no identity found".
#
SIGNCERT?= -
ifneq ($(SIGNCERT),-)
ifeq ($(shell security find-identity -v 2>/dev/null | grep -c $(SIGNCERT)),0)
$(warning SIGNCERT '$(SIGNCERT)' is not a valid signing identity in the keychain; falling back to ad-hoc signing)
SIGNCERT:= -
endif
endif

#
# NAME-COLLISION WARNING: XNU already ships a `devfs' VFS (the thing that
# creates /dev nodes at boot). Our bundle id and lock group are unique
# (com.beako.filesystems.devfs / com.beako.filesystems.devfs.lckgrp), but the
# repo's own `make stat' / `make unload' targets must grep kextstat for the
# BUNDLE ID, not the bare name - `kextstat | grep devfs' would also match the
# kernel's own devfs VFS component and unload the wrong thing.
#
