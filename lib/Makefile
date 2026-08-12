#
# All-in-one Makefile
#
# DevFS M0: the kext links only libkext (uuid/assert helpers) and the SDK's
# libkmod. libsbuf / libbsdmalloc are added back when the data-formatting
# nodes (ashmem, etc.) arrive with their consumers.
#

MAKE=make

all: debug

debug:
	$(MAKE) -C libkext

release: TARGET=release
release: debug

clean:
	$(MAKE) -C libkext clean

.PHONY: all debug release clean