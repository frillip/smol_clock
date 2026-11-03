/*
 * The PIC's state as text, from the SML messages it sends, and names for the
 * SML wire codes.
 */
#ifndef PIC_REPORT_H
#define PIC_REPORT_H

#include <Arduino.h>
#include <stdint.h>

const char *pic_report_command_name(uint16_t command);
const char *pic_report_result_name(uint16_t result);
const char *pic_report_error_name(uint16_t error);
const char *pic_report_source_name(uint8_t source);
const char *pic_report_lock_name(uint8_t lock);
const char *pic_report_tz_source_name(uint8_t source);
// BOOT-STATUS mode: WINDOW, HELD, NO_APP, LOOP, APP.
const char *pic_report_boot_mode_name(uint8_t mode);
// The most telling of the SML_RESET_* bits, as text.
const char *pic_report_reset_name(uint16_t cause);
// "0.1.0" from major << 16 | minor << 8 | patch.
void pic_report_version(char *text, size_t size, uint32_t version);

// System (firmware, uptime, reset cause), time, synchroniser, loop,
// sources, GNSS and link. Compact unless verbose:
// a line or two each, the sources table, and nothing that is only counters.
void pic_report_print(Stream *output, bool verbose);
// STAT-SOURCES: the PIC's time sources, ranked (sources). Verbose
// adds the rank, each source's own figure, its run and its edge.
void pic_report_print_sources(Stream *output, bool verbose);
// The link counters only.
void pic_report_print_link(Stream *output);

#endif // PIC_REPORT_H
