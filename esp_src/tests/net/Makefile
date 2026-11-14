# Dual-stack fallback on the host. Run: make
CXX ?= g++
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra $(SAN)
SRC := ../../src

test: test_net_connect
	./test_net_connect

test_net_connect: test_net_connect.cpp $(SRC)/net/net_connect.cpp $(SRC)/util/time_text.cpp \
                  $(wildcard stubs/*.h stubs/lwip/*.h)
	$(CXX) $(CXXFLAGS) -I stubs -o $@ test_net_connect.cpp $(SRC)/net/net_connect.cpp \
	    $(SRC)/util/time_text.cpp

clean:
	rm -f test_net_connect

.PHONY: test clean
