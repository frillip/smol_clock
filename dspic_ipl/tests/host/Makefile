# Host tests for the IPL's selection and CRC-32 (../../ipl_select.c,
# ../../ipl_crc32.c), against a model of program memory. Run: make
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wconversion -Wno-sign-conversion
COMMON := ../../../common

all: test_ipl
	./test_ipl

test_ipl: test_ipl.c ../../ipl_select.c ../../ipl_crc32.c ../../ipl_select.h ../../ipl_flash.h \
          $(COMMON)/boot_crc.c $(COMMON)/sml.c $(COMMON)/boot_layout.h
	$(CC) $(CFLAGS) -o $@ test_ipl.c ../../ipl_select.c ../../ipl_crc32.c $(COMMON)/boot_crc.c \
	      $(COMMON)/sml.c

clean:
	rm -f test_ipl

.PHONY: all clean
