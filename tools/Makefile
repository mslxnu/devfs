CC=     cc
CFLAGS= -O2 -Wall -Wextra -std=c99 -g -fPIE -I../include
PROGS=  binder-probe

all: $(PROGS)

binder-probe: binder-probe.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
	rm -rf *.dSYM

.PHONY: all clean