# NTP server selection and statistics, and ntp.cpp against fake servers, on the host. Run: make
CXX ?= g++
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra $(SAN)
SRC := ../../src
NTP := $(SRC)/timing/ntp.cpp $(SRC)/timing/esp_clock.cpp $(SRC)/timing/temp_comp.cpp \
       $(SRC)/timing/ntp_select.cpp \
       $(SRC)/timing/ntp_stats.cpp $(SRC)/timing/ntp_packet.cpp $(SRC)/util/time_text.cpp

test: test_ntp_select test_ntp_stats test_ntp test_esp_clock test_temp_comp test_phase_stats
	./test_ntp_select
	./test_ntp_stats
	./test_ntp
	./test_esp_clock
	./test_temp_comp
	./test_phase_stats

test_ntp_select: test_ntp_select.cpp $(SRC)/timing/ntp_select.cpp $(SRC)/timing/ntp_select.h
	$(CXX) $(CXXFLAGS) -o $@ test_ntp_select.cpp $(SRC)/timing/ntp_select.cpp

test_ntp_stats: test_ntp_stats.cpp $(SRC)/timing/ntp_stats.cpp $(SRC)/timing/ntp_stats.h
	$(CXX) $(CXXFLAGS) -o $@ test_ntp_stats.cpp $(SRC)/timing/ntp_stats.cpp

test_ntp: test_ntp.cpp $(NTP) $(wildcard stubs/*.h)
	$(CXX) $(CXXFLAGS) -I stubs -I $(SRC)/pic/sml -o $@ test_ntp.cpp $(NTP)

test_esp_clock: test_esp_clock.cpp $(SRC)/timing/esp_clock.cpp $(SRC)/timing/esp_clock.h \
                $(SRC)/timing/ntp_stats.cpp $(SRC)/timing/ntp_stats.h
	$(CXX) $(CXXFLAGS) -o $@ test_esp_clock.cpp $(SRC)/timing/esp_clock.cpp $(SRC)/timing/ntp_stats.cpp \
	    $(SRC)/timing/temp_comp.cpp

test_temp_comp: test_temp_comp.cpp $(SRC)/timing/esp_clock.cpp $(SRC)/timing/esp_clock.h \
                $(SRC)/timing/temp_comp.cpp $(SRC)/timing/temp_comp.h
	$(CXX) $(CXXFLAGS) -o $@ test_temp_comp.cpp $(SRC)/timing/esp_clock.cpp \
	    $(SRC)/timing/temp_comp.cpp

test_phase_stats: test_phase_stats.cpp $(SRC)/timing/phase_stats.cpp $(SRC)/timing/phase_stats.h
	$(CXX) $(CXXFLAGS) -o $@ test_phase_stats.cpp $(SRC)/timing/phase_stats.cpp

clean:
	rm -f test_ntp_select test_ntp_stats test_ntp test_esp_clock test_temp_comp test_phase_stats

.PHONY: test clean
