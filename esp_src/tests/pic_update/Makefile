# The PIC update on the ESP, on the host: the staged files (SHA-256, file
# headers, the picfw slots) and the update procedure against the SPL's own
# core from dspic_spl, with a stand-in for the PIC's application. Run: make
CC ?= gcc
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS ?= -std=c99 -O1 -g -Wall -Wextra -Wno-sign-conversion $(SAN)
SRC := ../../src
SML := $(SRC)/pic/sml
SPL := ../../../dspic_spl

STORE := $(SRC)/util/sha256.c $(SRC)/pic/pic_file.c $(SRC)/pic/pic_store.c $(SML)/boot_crc.c \
         $(SML)/sml.c
PIC := $(SPL)/spl_core.c $(SPL)/tests/host/port_host.c

test: test_pic_store test_pic_update test_spl_update test_spl_update_old
	python3 ../../../tools/sync_common.py --check # the copies of shared files, as the originals
	./test_pic_store
	./test_pic_update
	./test_spl_update
	./test_spl_update_old

test_pic_store: test_pic_store.c $(STORE) $(wildcard $(SRC)/pic/*.h $(SRC)/util/*.h)
	$(CC) $(CFLAGS) -o $@ test_pic_store.c $(STORE)

test_pic_update: test_pic_update.c $(STORE) $(SRC)/pic/pic_update.c $(PIC) \
                 $(wildcard $(SRC)/pic/*.h $(SPL)/*.h)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ test_pic_update.c $(SRC)/pic/pic_update.c $(STORE) $(PIC)

# The SPL update end to end: the IPL's choice, the SPL's core and the
# application's SPL writer, all on the SPL's host flash model.
IPL := ../../../dspic_ipl
APP := ../../../dspic_src/esp
test_spl_update: test_spl_update.c $(STORE) $(SRC)/pic/spl_update.c $(PIC) $(APP)/spl_writer.c \
                 $(IPL)/ipl_select.c $(IPL)/ipl_crc32.c $(wildcard $(SRC)/pic/*.h $(SPL)/*.h $(APP)/*.h)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ test_spl_update.c $(SRC)/pic/spl_update.c $(STORE) \
	      $(PIC) $(APP)/spl_writer.c $(IPL)/ipl_select.c $(IPL)/ipl_crc32.c

# The same against an older application, which confirms only after
# APP_HEALTHY_S: the ESP's CONFIRM at APP_CONFIRM_S is refused, and sent again.
test_spl_update_old: test_spl_update.c $(STORE) $(SRC)/pic/spl_update.c $(PIC) $(APP)/spl_writer.c \
                     $(IPL)/ipl_select.c $(IPL)/ipl_crc32.c $(wildcard $(SRC)/pic/*.h $(SPL)/*.h $(APP)/*.h)
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -DSPL_WRITER_CONFIRM_S=APP_HEALTHY_S -o $@ test_spl_update.c \
	      $(SRC)/pic/spl_update.c $(STORE) $(PIC) $(APP)/spl_writer.c $(IPL)/ipl_select.c $(IPL)/ipl_crc32.c

clean:
	rm -f test_pic_store test_pic_update test_spl_update test_spl_update_old

.PHONY: test clean
