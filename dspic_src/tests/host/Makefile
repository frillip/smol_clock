# Host tests for timing/, gnss/, sync/, esp/, ui/, console/, settings/ and drivers/. Run: make
# To compare the estimator against the original files as well:
#   make sim_old ORIGINAL=/path/to/unmodified/master-epoch-test && make
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wconversion -Wno-sign-conversion
ORIGINAL ?=
T := ../../timing
D := ../../drivers
U := ../../ui
C := ../../console
G := ../../gnss
S := ../../sync
GNSS_SOURCES := $(C)/text.c host_console.c ../../platform/utc.c $(G)/ubx_parser.c $(G)/ubx_decode.c $(G)/ubx_encode.c $(G)/gnss_epoch.c \
                $(G)/gnss_detect.c $(G)/gnss.c

all: test_text test_utc test_numeric sim_new test_epoch_scheduler test_acquire test_sync test_sources test_monitor test_console test_gnss test_uart2 \
     test_display test_scheduler test_sml test_timezone test_esp_link test_esp_report test_i2c test_rtc \
     test_store test_settings test_epoch_resume test_brightness test_veml6040 test_controls test_buzzer test_beeps test_alarm test_menu test_set_time test_boot_layout test_boot test_fault test_spl_writer
	./test_text
	./test_utc
	./test_numeric
	./sim_new
	./test_epoch_scheduler
	./test_acquire
	./test_sync
	./test_sync_link
	./test_sources
	./test_monitor
	./test_console
	./test_gnss
	./test_uart2
	./test_display
	./test_scheduler
	./test_sml
	./test_boot_layout
	./test_boot
	./test_fault
	./test_spl_writer
	./test_timezone
	./test_esp_link
	./test_esp_report
	./test_i2c
	./test_rtc
	./test_store
	./test_settings
	./test_epoch_resume
	./test_brightness
	./test_veml6040
	./test_controls
	./test_buzzer
	./test_beeps
	./test_alarm
	./test_menu
	./test_set_time
	@if [ -f sim_old ]; then ./sim_old; fi

# Interrupt handlers compile on the host once the dsPIC attributes go.
%_host.c: $(T)/%.c
	sed -E 's/__attribute__ *\(\( *interrupt *, *no_auto_psv *\)\)//' $< > $@
uart%_host.c: $(D)/uart%.c
	sed -E 's/__attribute__ *\(\( *interrupt *, *no_auto_psv *\)\)//' $< > $@
buzzer_host.c: $(D)/buzzer.c
	sed -E 's/__attribute__ *\(\( *interrupt *, *no_auto_psv *\)\)//' $< > $@
i2c1_host.c: $(D)/i2c1.c
	sed -E 's/__attribute__ *\(\( *interrupt *, *no_auto_psv *\)\)//' $< > $@

# The RTC source against a model of the PCF8563 and of time.
test_rtc: test_rtc.c $(S)/source_rtc.c ../../platform/utc.c $(C)/debug.c $(C)/text.c host_console.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -o $@ $^ -lm

# The I2C1 master and the PCF8563 against a model of the module and bus.
test_i2c: test_i2c.c i2c1_host.c $(D)/pcf8563.c ../../platform/utc.c fake_i2c/xc.h
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I fake_i2c -I $(D) -o $@ test_i2c.c i2c1_host.c \
	    $(D)/pcf8563.c ../../platform/utc.c

# The fast report formatter against printf.
test_text: test_text.c $(C)/text.c
	$(CC) $(CFLAGS) -o $@ $^

# Calendar and leap seconds, every day 1970-2106.
test_utc: test_utc.c ../../platform/utc.c
	$(CC) $(CFLAGS) -o $@ $^

# Fixed-point estimator and servo against double precision.
test_numeric: test_numeric.c $(T)/kalman.c $(T)/servo.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

sim_new: sim_main.c adapter_new.c $(T)/kalman.c $(T)/servo.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

sim_old: sim_main.c adapter_old.c $(ORIGINAL)/kalman.c $(ORIGINAL)/epoch_servo.c
	$(CC) -std=c99 -O2 -I $(ORIGINAL) -o $@ $^ -lm

# Epoch timer, scheduler and OC1 output against a cycle-level model.
test_epoch_scheduler: test_epoch_scheduler.c epoch_timer_host.c $(T)/epoch_output.c fake_sfr.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I fake_xc -I $(T) -o $@ $^

# The control loop, closed through a model of the clock and receiver.
test_acquire: test_acquire.c $(T)/epoch_control.c $(T)/kalman.c $(T)/servo.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -o $@ $^ -lm

# Source selection, state machine and time of day, closed through the same
# model, with a scripted GNSS source.
test_sync: test_sync.c $(S)/sync.c $(S)/sync_source.c $(S)/sync_report.c ../../platform/utc.c \
           $(C)/text.c host_console.c \
           $(T)/epoch_control.c $(T)/kalman.c $(T)/servo.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -DSYNC_SYNCHRONISED_LIMIT_NS=4000UL -o $@ $^ -lm
	$(CC) -std=gnu99 -O1 -Wall -Wextra -DSYNC_SYNCHRONISED_LIMIT_NS=4000UL -DLEAP_FROM_LINK=1 \
	      -o $@_link $^ -lm

# Selection between GNSS, NTP and holdover, closed through a model of both
# PPS inputs, with the real edge pairing.
test_sources: test_sources.c $(S)/sync.c $(S)/sync_source.c $(S)/sync_report.c $(S)/source_edge.c \
              ../../platform/utc.c $(C)/text.c host_console.c \
              $(T)/epoch_control.c $(T)/kalman.c $(T)/servo.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -o $@ $^ -lm

# The PPS watch's change-notification handler, with edges arriving mid-handler.
test_monitor: test_monitor.c pps_monitor_host.c fake_monitor/xc.h
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I fake_monitor -I $(T) -o $@ test_monitor.c pps_monitor_host.c

# UART1 driver and the printf write() hook, against a model of the UART.
test_console: test_console.c uart1_host.c $(C)/console.c fake_uart/xc.h
	$(CC) -std=gnu99 -O1 -Wall -Wextra -DCONSOLE_BAUD_RATE=115200UL -I fake_uart -I $(D) -o $@ \
	      test_console.c uart1_host.c $(C)/console.c

# GNSS stack against a simulated u-blox receiver over a stand-in UART2.
test_gnss: test_gnss.c host_uart2.c sim_receiver.c memmove_count.c fake_gnss_sfr.c $(GNSS_SOURCES) \
           $(S)/source_gnss.c $(S)/source_edge.c
	$(CC) -std=gnu99 -O1 -g -Wall -Wextra -I fake_gnss -fno-builtin-memmove -Wl,--wrap=memmove -o $@ $^

# UART2 driver against a modelled UART peripheral.
test_uart2: test_uart2.c uart2_host.c fake_uart2/xc.h
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I fake_uart2 -I $(D) -o $@ test_uart2.c uart2_host.c

# Tube driver against the V2 wire format, and the display layer.
DISPLAY_SOURCES := ../../drivers/tubes.c $(U)/frame.c $(U)/display.c $(U)/timezone.c \
                   ../../platform/utc.c ../../../common/sml.c
test_display: test_display.c $(DISPLAY_SOURCES) fake_tubes/xc.h
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I fake_tubes -o $@ test_display.c $(DISPLAY_SOURCES)

# The cooperative scheduler against a stand-in epoch timer.
test_scheduler: test_scheduler.c $(T)/scheduler.c ../../platform/utc.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I ../.. -o $@ $^

# SML codec: CRC, message table, golden frames, parser recovery.
test_sml: test_sml.c ../../../common/sml.c ../../../common/sml_proto.h
	$(CC) $(CFLAGS) -o $@ test_sml.c ../../../common/sml.c

# The bootloader's layout, wire definitions and checksums (common/boot_*).
test_boot_layout: test_boot_layout.c ../../../common/boot_crc.c ../../../common/sml.c \
                  ../../../common/boot_layout.h ../../../common/boot_crc.h ../../../common/sml_proto.h
	$(CC) $(CFLAGS) -o $@ test_boot_layout.c ../../../common/boot_crc.c ../../../common/sml.c

# The SPL writer against a flash model, judged by the IPL's own selection.
IPL := ../../../dspic_ipl
test_spl_writer: test_spl_writer.c ../../esp/spl_writer.c ../../esp/spl_writer.h ../../../common/boot_crc.c \
                 ../../../common/sml.c $(IPL)/ipl_select.c $(IPL)/ipl_crc32.c ../../../common/boot_layout.h
	$(CC) $(CFLAGS) -o $@ test_spl_writer.c ../../esp/spl_writer.c ../../../common/boot_crc.c \
	      ../../../common/sml.c $(IPL)/ipl_select.c $(IPL)/ipl_crc32.c

# The fault record: watchdog resets and traps reported at the next start.
test_fault: test_fault.c ../../platform/fault.c ../../platform/fault.h ../../../common/boot_layout.h
	$(CC) $(CFLAGS) -o $@ test_fault.c ../../platform/fault.c

# The application's side of the boot mailbox.
test_boot: test_boot.c ../../platform/boot.c ../../platform/boot.h ../../../common/boot_layout.h
	$(CC) $(CFLAGS) -o $@ test_boot.c ../../platform/boot.c

# Local time from TZ-STATE schedules and manual settings.
test_timezone: test_timezone.c $(U)/timezone.c ../../../common/sml.c ../../platform/utc.c
	$(CC) $(CFLAGS) -o $@ $^

# The ESP link against a stand-in UART1 and stand-in reports, with the real
# settings and store on the flash model.
SETTINGS_SOURCES := ../../settings/settings.c ../../settings/store.c fake_flash/fake_flash.c \
                    fake_control.c fake_source_rtc.c $(T)/kalman.c
test_esp_link: test_esp_link.c ../../esp/esp_link.c ../../../common/sml.c $(U)/timezone.c \
               ../../platform/utc.c ../../platform/boot.c $(C)/debug.c $(C)/text.c $(SETTINGS_SOURCES) \
               ../../esp/spl_writer.c ../../../common/boot_crc.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I fake_esp -o $@ $^

# The settings store against a model of the flash: power cut at every point.
test_store: test_store.c ../../settings/store.c fake_flash/fake_flash.c ../../../common/sml.c
	$(CC) $(CFLAGS) -o $@ $^

# The settings at start-up and when the flash fails.
test_settings: test_settings.c $(SETTINGS_SOURCES) $(U)/timezone.c $(C)/debug.c $(C)/text.c \
               ../../../common/sml.c ../../platform/utc.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -o $@ $^

# Brightness from the light, the manual level, and the fade.
test_brightness: test_brightness.c $(U)/brightness.c
	$(CC) $(CFLAGS) -o $@ $^

# The buzzer's patterns and priorities, against a model of Timer5.
test_buzzer: test_buzzer.c buzzer_host.c fake_buzzer/xc.h
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I fake_buzzer -I $(D) -o $@ test_buzzer.c buzzer_host.c

# When the chimes and clicks sound.
test_beeps: test_beeps.c $(U)/beeps.c
	$(CC) $(CFLAGS) -o $@ $^

# The alarm: once a day at its local time, DST and steps, and stopping it.
test_alarm: test_alarm.c $(U)/alarm.c
	$(CC) $(CFLAGS) -o $@ $^

# The time set by hand (the menu's tIME), against the real sync and epoch control.
test_set_time: test_set_time.c $(S)/sync.c $(S)/sync_source.c $(S)/sync_report.c ../../platform/utc.c \
               $(C)/text.c host_console.c $(T)/epoch_control.c $(T)/kalman.c $(T)/servo.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -o $@ $^ -lm

# The menu against stand-in settings, display, synchroniser and ESP.
test_menu: test_menu.c ../../menu/menu.c $(U)/frame.c ../../platform/utc.c
	$(CC) $(CFLAGS) -o $@ $^

# The button and switch: debouncing, short and long presses.
test_controls: test_controls.c $(U)/controls.c
	$(CC) $(CFLAGS) -o $@ $^

# The light sensor against a stand-in I2C1.
test_veml6040: test_veml6040.c $(D)/veml6040.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -o $@ $^

# The epoch timer catching up after a flash stall.
test_epoch_resume: test_epoch_resume.c epoch_timer_host.c $(T)/epoch_output.c fake_sfr.c
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I fake_xc -I $(T) -o $@ $^

# The SML payloads built from the clock's state, against stand-in states.
test_esp_report: test_esp_report.c ../../esp/esp_report.c $(U)/timezone.c ../../../common/sml.c \
                 ../../platform/utc.c ../../platform/boot.c ../../platform/fault.c ../../esp/spl_writer.c fake_report/xc.h
	$(CC) -std=gnu99 -O1 -Wall -Wextra -I fake_report -o $@ test_esp_report.c \
	      ../../esp/esp_report.c $(U)/timezone.c ../../../common/sml.c ../../platform/utc.c \
	      ../../platform/boot.c ../../platform/fault.c ../../esp/spl_writer.c ../../../common/boot_crc.c

clean:
	rm -f test_spl_writer test_fault test_rtc test_i2c i2c1_host.c test_text test_sync_link test_sources test_monitor pps_monitor_host.c test_utc test_numeric sim_new sim_old test_epoch_scheduler test_acquire test_sync test_console test_gnss test_uart2 test_display test_scheduler test_sml test_boot_layout test_boot test_timezone test_esp_link test_esp_report \
	      test_store test_settings test_epoch_resume test_brightness test_veml6040 test_controls test_buzzer test_beeps test_alarm test_menu test_set_time buzzer_host.c epoch_timer_host.c uart1_host.c uart2_host.c

.PHONY: all clean
