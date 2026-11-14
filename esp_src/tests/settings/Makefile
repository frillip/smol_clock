# The ESP's settings against a fake NVS. Run: make
CXX ?= g++
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra $(SAN)
SRC := ../../src

test: test_user_prefs
	./test_user_prefs

test_user_prefs: test_user_prefs.cpp $(SRC)/settings/user_prefs.cpp $(SRC)/util/time_text.cpp \
                 $(wildcard stubs/*.h)
	$(CXX) $(CXXFLAGS) -I stubs -o $@ test_user_prefs.cpp $(SRC)/settings/user_prefs.cpp \
	    $(SRC)/util/time_text.cpp

clean:
	rm -f test_user_prefs

.PHONY: test clean
