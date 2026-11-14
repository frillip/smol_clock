# The update manifest, format 2: checked and the configuration chosen, with
# the real ArduinoJson (git submodule update --init). Run: make
CXX ?= g++
CC ?= gcc
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra $(SAN)
CFLAGS ?= -std=c99 -O1 -g -Wall -Wextra $(SAN)
SRC := ../../src
JSON := ../../libraries/ArduinoJson/src

test: test_manifest
	./test_manifest

sha256.o: $(SRC)/util/sha256.c $(SRC)/util/sha256.h
	$(CC) $(CFLAGS) -c -o $@ $<

test_manifest: test_manifest.cpp $(SRC)/net/manifest.cpp $(SRC)/net/manifest.h sha256.o
	$(CXX) $(CXXFLAGS) -I $(JSON) -o $@ test_manifest.cpp $(SRC)/net/manifest.cpp sha256.o

clean:
	rm -f test_manifest sha256.o

.PHONY: test clean
