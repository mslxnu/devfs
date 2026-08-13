CC=     cc
CFLAGS= -O2 -Wall -Wextra -std=gnu11 -g -fPIE -I../include
PROGS=  binder-probe binder-abi-test

all: $(PROGS)

binder-probe: binder-probe.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
	rm -rf *.dSYM

.PHONY: all clean
binder-abi-test: binder-abi-test.c ../include/fs/devfs/binder.h
	$(CC) $(CFLAGS) -o $@ $<
