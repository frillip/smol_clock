# Host tests for the SPL's core (../../spl_core.c) against a flash model
# (port_host.c), and spl_sim, the SPL on a pipe for tools/pic_boot.py's
# tests. Run: make
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wconversion -Wno-sign-conversion
COMMON := ../../../common
CORE := ../../spl_core.c ../../spl_core.h ../../spl_port.h $(COMMON)/boot_crc.c $(COMMON)/sml.c \
        $(COMMON)/boot_layout.h $(COMMON)/sml_proto.h
SOURCES := ../../spl_core.c $(COMMON)/boot_crc.c $(COMMON)/sml.c

all: test_spl spl_sim
	./test_spl

test_spl: test_spl.c port_host.c port_host.h $(CORE)
	$(CC) $(CFLAGS) -o $@ test_spl.c port_host.c $(SOURCES)

# Built as the SPL is, without sml.c's message table: proves the core needs none.
spl_sim: spl_sim.c port_host.c port_host.h $(CORE)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -DSML_NO_MESSAGES -o $@ spl_sim.c port_host.c $(SOURCES)

clean:
	rm -f test_spl spl_sim

.PHONY: all clean
