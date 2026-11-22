# The console commands and the PIC link on the host. Run: make
CXX ?= g++
CC ?= gcc
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra $(SAN)
CFLAGS ?= -std=c99 -O1 -g -Wall -Wextra $(SAN)
SRC := ../../src
INCLUDES := -I stubs -I $(SRC)/pic/sml

test: test_commands
	python3 ../../../tools/sync_common.py --check # the copies of shared files, as the originals
	./test_commands

sha256.o: $(SRC)/util/sha256.c
	$(CC) $(CFLAGS) -c -o $@ $<

sml.o: $(SRC)/pic/sml/sml.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

test_commands: test_commands.cpp sml.o sha256.o $(SRC)/console/commands.cpp $(SRC)/pic/pic_link.cpp \
               $(SRC)/pic/pic_report.cpp $(SRC)/pic/sml_stream.cpp $(SRC)/util/parse.cpp \
               $(SRC)/util/time_text.cpp $(SRC)/util/loop_timing.cpp $(SRC)/net/telnet_auth.cpp \
               $(SRC)/timing/temp_comp.cpp $(SRC)/util/sha256.c stubs/Arduino.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ test_commands.cpp sml.o $(SRC)/console/commands.cpp \
	    $(SRC)/pic/pic_link.cpp $(SRC)/pic/pic_report.cpp $(SRC)/pic/sml_stream.cpp \
	    $(SRC)/util/parse.cpp $(SRC)/util/time_text.cpp $(SRC)/util/loop_timing.cpp \
	    $(SRC)/net/telnet_auth.cpp $(SRC)/timing/temp_comp.cpp sha256.o

clean:
	rm -f test_commands sml.o sha256.o

.PHONY: test clean
