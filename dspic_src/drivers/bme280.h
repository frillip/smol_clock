/*
 * Bosch BME280 temperature, pressure and humidity sensor on I2C1 (address
 * 0x76), as fitted to V2. On the board it reads the board, not the room:
 * the power supplies warm it well above ambient. Its use is the change in
 * temperature, to set against the oscillator's frequency. Ported from V2's
 * bme280.c onto drivers/i2c1.h, as the VEML6040 was: nothing waits.
 *
 *   - Found by its chip ID (BME280_CHIP_ID), then its calibration read
 *     (two blocks), then configured: normal mode, a measurement every
 *     second (1000 ms standby), temperature oversampled x16, pressure x4,
 *     humidity x1, IIR filter x4. About 0.1 mW: no self-heating to speak
 *     of. The configuration is written with the chip asleep first, as the
 *     data sheet ignores config writes in normal mode, and read back.
 *   - Read back every BME280_CHECK_INTERVAL_MS and rewritten if it differs,
 *     and after the chip has not answered: a reset puts it back to sleep.
 *     Data read while it is not known to be configured is not used, nor
 *     data the chip has not measured yet (0x80000, its reset value).
 *   - Read every BME280_READ_INTERVAL_MS; a missing chip is tried every
 *     BME280_DETECT_INTERVAL_MS.
 *   - Compensation: the data sheet's 32-bit integer formulas (section 8.2,
 *     4.2.3), pressure included, so no 64-bit arithmetic on the dsPIC.
 *     The calibration is kept as 32-bit values, so with 16-bit int every
 *     product is still 32-bit.
 *
 * One request at a time, finished by bme280_service() from the main loop.
 */
#ifndef BME280_H
#define BME280_H

#include <stdbool.h>
#include <stdint.h>

#include "i2c1.h"

#define BME280_ADDRESS 0x76U
#define BME280_CHIP_ID 0x60U

// Registers.
#define BME280_REG_CALIB_A   0x88U // dig_T1..dig_P9, (0xA0 unused), dig_H1: 26 bytes
#define BME280_REG_ID        0xD0U
#define BME280_REG_CALIB_B   0xE1U // dig_H2..dig_H6: 7 bytes
#define BME280_REG_CTRL_HUM  0xF2U
#define BME280_REG_STATUS    0xF3U
#define BME280_REG_CTRL_MEAS 0xF4U
#define BME280_REG_CONFIG    0xF5U
#define BME280_REG_DATA      0xF7U // press, temp (3 bytes each), hum (2)

#define BME280_CALIB_A_BYTES 26U
#define BME280_CALIB_B_BYTES 7U
#define BME280_DATA_BYTES    8U
#define BME280_CTRL_BYTES    4U // ctrl_hum, status, ctrl_meas, config

#define BME280_MODE_SLEEP  0x00U
#define BME280_MODE_NORMAL 0x03U

#define BME280_OSRS_X1  0x01U
#define BME280_OSRS_X4  0x03U
#define BME280_OSRS_X16 0x05U

#define BME280_FILTER_X4 0x02U

#define BME280_STANDBY_1000MS 0x05U

// The settings: humidity x1; temperature x16, pressure x4, normal mode;
// 1000 ms standby, filter x4 (SPI 3-wire off).
#define BME280_CTRL_HUM_VALUE BME280_OSRS_X1
#define BME280_CTRL_MEAS_VALUE \
    ((uint8_t)((BME280_OSRS_X16 << 5) | (BME280_OSRS_X4 << 2) | BME280_MODE_NORMAL))
#define BME280_CONFIG_VALUE ((uint8_t)((BME280_STANDBY_1000MS << 5) | (BME280_FILTER_X4 << 2)))

// Not measured since reset (or skipped): the data registers' reset values.
#define BME280_SKIPPED_20 0x80000UL
#define BME280_SKIPPED_16 0x8000UL

// The board runs this much above the room (V2's figure, from a comparison
// with a thermometer, for one board in one place): ambient = board + this.
// A rough guide only.
#define BME280_AMBIENT_OFFSET_CENTIC (-1280L)

// Retry a chip that did not answer this often.
#define BME280_DETECT_INTERVAL_MS 1000UL
// Read a present chip this often: once per measurement.
#define BME280_READ_INTERVAL_MS 1000UL
// Read the configuration back this often.
#define BME280_CHECK_INTERVAL_MS 60000UL

typedef struct
{
    int32_t t1, t2, t3;
    int32_t p1, p2, p3, p4, p5, p6, p7, p8, p9;
    int32_t h1, h2, h3, h4, h5, h6;
} Bme280Calibration;

typedef struct
{
    bool temperature_valid;
    bool pressure_valid;
    bool humidity_valid;
    int32_t temperature_centic;     // 0.01 C, -4000..8500
    uint32_t pressure_pa;           // Pa, 30000..110000
    uint16_t humidity_centipercent; // 0.01 %RH, 0..10000
} Bme280Reading;

typedef enum
{
    BME280_NONE = 0,    // no request yet
    BME280_IDENTIFYING, // reading the chip ID
    BME280_CALIBRATING, // reading the calibration (two blocks)
    BME280_WRITING,     // writing the configuration
    BME280_CHECKING,    // reading the configuration back
    BME280_READING,     // reading the data
    BME280_DONE,        // the last request worked
    BME280_FAILED,      // the last request failed: see last_result
} Bme280Request;

typedef struct
{
    bool present;    // something answers at the address
    bool identified; // ... with BME280_CHIP_ID
    bool calibrated;
    bool configured; // the configuration was read back as written
    uint8_t chip_id; // as read
    Bme280Reading reading;
    uint32_t raw_temperature; // 20-bit counts, as last read
    uint32_t raw_pressure;
    uint32_t raw_humidity; // 16-bit
    uint32_t read_ms;      // the millisecond clock when that read finished
    Bme280Request request;
    I2cResult last_result;
    uint32_t reads;
    uint32_t writes;
    uint32_t mismatches; // configuration read back different (reset?)
    uint32_t failures;
} Bme280Status;

// Finds and configures the sensor (if it answers). After i2c1_init().
void bme280_init(void);

// Every pass of the main loop: finishes requests and starts the next.
// Returns true when a data read has just finished.
bool bme280_service(uint32_t now_ms);

// True once, after the first measurement once configured: worth a
// bme280_print, if the caller wants one.
bool bme280_report_due(void);

// Periodic reads on or off (on by default): off while another user of the
// bus needs it to itself (the RTC source).
void bme280_set_auto_read(bool enabled);

const Bme280Status *bme280_status(void);
// One line: the readings and how fresh they are, and any failure. verbose:
// the raw counts and the counts of transfers too.
void bme280_print(uint32_t now_ms, bool verbose);

// The calibration from its two blocks, as read.
void bme280_decode_calibration(const uint8_t a[BME280_CALIB_A_BYTES],
                               const uint8_t b[BME280_CALIB_B_BYTES], Bme280Calibration *cal);
// Raw counts to a reading. A skipped measurement is not valid; pressure and
// humidity need the temperature.
void bme280_compensate(const Bme280Calibration *cal, uint32_t raw_temperature,
                       uint32_t raw_pressure, uint32_t raw_humidity, Bme280Reading *reading);

#endif // BME280_H
