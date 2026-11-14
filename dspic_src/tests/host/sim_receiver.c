#include "sim_receiver.h"
#include "host_uart2.h"
#include "../../gnss/ubx_encode.h"
#include "../../gnss/ubx_parser.h"
#include "../../drivers/uart2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Real MON-VER payload from a NEO-M9N (etc/gnss_config.txt).
static const char mon_ver_hex[] =
    "45 58 54 20 43 4F 52 45 20 34 2E 30 34 20 28 37 66 38 39 66 37 29 00 00 00 00 00 00 00 00 "
    "30 30 31 39 30 30 30 30 00 00 52 4F 4D 20 42 41 53 45 20 30 78 31 31 38 42 32 30 36 30 00 "
    "00 00 00 00 00 00 00 00 00 00 46 57 56 45 52 3D 53 50 47 20 34 2E 30 34 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 00 00 50 52 4F 54 56 45 52 3D 33 32 2E 30 31 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 00 00 4D 4F 44 3D 4E 45 4F 2D 4D 39 4E 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 00 00 47 50 53 3B 47 4C 4F 3B 47 41 4C 3B 42 44 53 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 00 00 53 42 41 53 3B 51 5A 53 53 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 00 00";

#define MAX_KEYS 64
typedef struct
{
    uint32_t key;
    uint32_t value;
} KeyValue;
static KeyValue ram[MAX_KEYS];
static int key_count;

static SimReceiverConfig cfg;
static uint32_t baud;
// The receiver's own framer: accepts any well-formed frame, including the
// zero-length polls and CFG messages the dsPIC parser deliberately ignores.
static uint8_t rx_buffer[1024];
static uint16_t rx_used;
static uint8_t last_layers;
static uint32_t flash_writes;
// Time runs on continuously in pps_utc; the UTC label of a second differs
// from it once a scheduled leap second has passed (see label_of).
static int64_t pps_utc;      // second the receiver is in, continuous
static int64_t pps_edge_utc; // UTC label of the last pulse actually output
static bool pps_edge_leap;   // ... which was an inserted 23:59:60
static int64_t leap_event;   // midnight after the scheduled leap second
static int8_t leap_change;   // 0: none scheduled
static GnssPpsReference pps;
static bool drop_next_pps;
static uint32_t pending_epoch_ms; // when the epoch messages go out
static bool epoch_pending;
static uint32_t epoch_counter;
static uint32_t pending_baud_switch;
static bool baud_switch_pending;
static int32_t last_pps_qerr_ps;          // qErr of the last pulse actually output
static int32_t epoch_mark_offset_ns = 37; // where our epoch edge lands, per the receiver

// The receiver's quantisation error for the pulse at this UTC second: a
// deterministic sawtooth-like pattern, a few ns either way.
static int32_t qerr_for(int64_t utc)
{
    return (int32_t)((utc * 7919) % 8001) - 4000;
}


static KeyValue *find(uint32_t key)
{
    for (int i = 0; i < key_count; i++)
        if (ram[i].key == key)
            return &ram[i];
    return NULL;
}


static void set_key(uint32_t key, uint32_t value)
{
    KeyValue *kv = find(key);
    if (!kv)
    {
        kv = &ram[key_count++];
        kv->key = key;
    }
    kv->value = value;
}


static bool key_known(uint32_t key)
{
    if (key == UBX_CFG_MSGOUT_TIM_TM2_UART1 && !cfg.supports_tim_tm2)
        return false;
    return find(key) != NULL;
}


// Sends a frame to the dsPIC: clean if the baud rates match, else noise
// with framing errors (what a UART sees at the wrong rate).
static void emit(uint8_t class_id, uint8_t message_id, const uint8_t *payload, uint16_t length)
{
    uint8_t frame[700];
    uint16_t n = ubx_encode_frame(class_id, message_id, payload, length, frame, sizeof(frame));
    if (host_uart2_baud() == baud)
    {
        host_uart2_inject_rx(frame, n);
    }
    else
    {
        for (uint16_t i = 0; i < n; i++)
            frame[i] = (uint8_t)rand();
        host_uart2_inject_rx(frame, (uint16_t)(n / 3U));
        host_uart2_inject_error(UART2_RX_ERROR_FRAMING);
    }
}


static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}


static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}


static void ack(bool ok, uint8_t message_id)
{
    uint8_t payload[2] = {UBX_CLASS_CFG, message_id};
    emit(UBX_CLASS_ACK, ok ? UBX_ID_ACK_ACK : UBX_ID_ACK_NAK, payload, 2);
}


static void handle_valset(const uint8_t *payload, uint16_t length)
{
    uint8_t layers = payload[1];
    const uint8_t *p = payload + 4, *end = payload + length;
    uint32_t new_baud = 0;

    last_layers = layers;
    if ((layers & UBX_CFG_LAYER_FLASH) && !cfg.has_flash)
    {
        ack(false, UBX_ID_CFG_VALSET);
        return;
    }
    // Validate everything first: VALSET is all-or-nothing.
    for (const uint8_t *q = p; q < end;)
    {
        uint32_t key =
            (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
        uint8_t size = (uint8_t)UBX_CFG_KEY_VALUE_SIZE(key);
        if (!key_known(key) || size == 0)
        {
            ack(false, UBX_ID_CFG_VALSET);
            return;
        }
        q += 4 + size;
    }
    while (p < end)
    {
        uint32_t key =
            (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        uint8_t size = (uint8_t)UBX_CFG_KEY_VALUE_SIZE(key);
        uint32_t value = 0;
        for (int i = 0; i < size && i < 4; i++)
            value |= (uint32_t)p[4 + i] << (8 * i);
        if (key == UBX_CFG_UART1_BAUDRATE)
            new_baud = value;
        else
            set_key(key, value);
        p += 4 + size;
    }
    if (layers & UBX_CFG_LAYER_FLASH)
        flash_writes++;
    if (new_baud && new_baud != baud)
    {
        set_key(UBX_CFG_UART1_BAUDRATE, new_baud);
        if (cfg.lose_baud_ack)
        {
            baud = new_baud;
            ack(true, UBX_ID_CFG_VALSET);
        }
        else
        {
            ack(true, UBX_ID_CFG_VALSET);
            pending_baud_switch = new_baud;
            baud_switch_pending = true;
        }
    }
    else
    {
        if (new_baud)
            set_key(UBX_CFG_UART1_BAUDRATE, new_baud);
        ack(true, UBX_ID_CFG_VALSET);
    }
}


static void handle_valget(const uint8_t *payload, uint16_t length)
{
    uint8_t out[600];
    uint16_t n = 4;
    out[0] = 1;
    out[1] = payload[1];
    out[2] = 0;
    out[3] = 0;
    for (const uint8_t *p = payload + 4; p < payload + length; p += 4)
    {
        uint32_t key =
            (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        uint8_t size = (uint8_t)UBX_CFG_KEY_VALUE_SIZE(key);
        KeyValue *kv = find(key);
        if (!key_known(key))
        {
            ack(false, UBX_ID_CFG_VALGET);
            return;
        }
        put_u32(out + n, key);
        n += 4;
        for (int i = 0; i < size; i++)
            out[n++] = (uint8_t)(kv->value >> (8 * i));
    }
    emit(UBX_CLASS_CFG, UBX_ID_CFG_VALGET, out, n);
    ack(true, UBX_ID_CFG_VALGET);
}


static void on_frame(uint8_t class_id, uint8_t message_id, const uint8_t *payload, uint16_t length,
                     uint32_t now_ms, void *context)
{
    (void)now_ms;
    (void)context;
    if (class_id == UBX_CLASS_MON && message_id == UBX_ID_MON_VER && length == 0)
    {
        uint8_t mv[300];
        uint16_t n = 0;
        for (const char *s = mon_ver_hex; *s;)
        {
            mv[n++] = (uint8_t)strtoul(s, (char **)&s, 16);
            while (*s == ' ')
                s++;
        }
        emit(UBX_CLASS_MON, UBX_ID_MON_VER, mv, n);
    }
    else if (class_id == UBX_CLASS_CFG && message_id == UBX_ID_CFG_VALSET)
        handle_valset(payload, length);
    else if (class_id == UBX_CLASS_CFG && message_id == UBX_ID_CFG_VALGET)
        handle_valget(payload, length);
}


static void receiver_feed(uint8_t byte)
{
    if (rx_used == sizeof(rx_buffer))
        rx_used = 0;
    rx_buffer[rx_used++] = byte;
    for (;;)
    {
        while (rx_used > 0 && rx_buffer[0] != UBX_SYNC_CHAR_1)
        {
            memmove(rx_buffer, rx_buffer + 1, --rx_used);
        }
        if (rx_used < 2)
            return;
        if (rx_buffer[1] != UBX_SYNC_CHAR_2)
        {
            memmove(rx_buffer, rx_buffer + 1, --rx_used);
            continue;
        }
        if (rx_used < 6)
            return;
        uint16_t length = (uint16_t)(rx_buffer[4] | rx_buffer[5] << 8);
        if (length + 8U > sizeof(rx_buffer))
        {
            memmove(rx_buffer, rx_buffer + 1, --rx_used);
            continue;
        }
        if (rx_used < length + 8U)
            return;
        uint8_t a, b;
        ubx_checksum(rx_buffer + 2, (uint16_t)(length + 4U), &a, &b);
        if (a == rx_buffer[length + 6] && b == rx_buffer[length + 7])
        {
            on_frame(rx_buffer[2], rx_buffer[3], rx_buffer + 6, length, 0, NULL);
            rx_used = (uint16_t)(rx_used - (length + 8U));
            memmove(rx_buffer, rx_buffer + length + 8, rx_used);
        }
        else
        {
            memmove(rx_buffer, rx_buffer + 1, --rx_used);
        }
    }
}


void sim_init(const SimReceiverConfig *config, int64_t start_unix_seconds)
{
    cfg = *config;
    baud = cfg.initial_baud;
    key_count = 0;
    // Factory defaults: UBX and NMEA on, no UBX messages.
    set_key(UBX_CFG_UART1_BAUDRATE, baud);
    set_key(UBX_CFG_UART1INPROT_UBX, 1);
    set_key(UBX_CFG_UART1INPROT_NMEA, 1);
    set_key(UBX_CFG_UART1INPROT_RTCM3X, 1);
    set_key(UBX_CFG_UART1OUTPROT_UBX, 1);
    set_key(UBX_CFG_UART1OUTPROT_NMEA, 1);
    set_key(UBX_CFG_RATE_MEAS, 1000);
    set_key(UBX_CFG_RATE_NAV, 1);
    set_key(UBX_CFG_RATE_TIMEREF, 1);
    set_key(UBX_CFG_NAVSPG_DYNMODEL, 0);
    set_key(UBX_CFG_MSGOUT_NAV_POSLLH_UART1, 0);
    set_key(UBX_CFG_MSGOUT_NAV_STATUS_UART1, 0);
    set_key(UBX_CFG_MSGOUT_NAV_TIMEUTC_UART1, 0);
    set_key(UBX_CFG_MSGOUT_NAV_CLOCK_UART1, 0);
    set_key(UBX_CFG_MSGOUT_NAV_TIMELS_UART1, 0);
    set_key(UBX_CFG_MSGOUT_NAV_EOE_UART1, 0);
    set_key(UBX_CFG_MSGOUT_TIM_TM2_UART1, 0);
    set_key(UBX_CFG_MSGOUT_TIM_TP_UART1, 0);
    set_key(UBX_CFG_TP_TP1_ENA, 1);
    set_key(UBX_CFG_TP_PULSE_DEF, 0);
    set_key(UBX_CFG_TP_PULSE_LENGTH_DEF, 1);
    set_key(UBX_CFG_TP_PERIOD_TP1, 1000000);
    set_key(UBX_CFG_TP_PERIOD_LOCK_TP1, 1000000);
    set_key(UBX_CFG_TP_LEN_TP1, 0);
    set_key(UBX_CFG_TP_LEN_LOCK_TP1, 100000);
    set_key(UBX_CFG_TP_SYNC_GNSS_TP1, 1);
    set_key(UBX_CFG_TP_USE_LOCKED_TP1, 1);
    set_key(UBX_CFG_TP_ALIGN_TO_TOW_TP1, 1);
    set_key(UBX_CFG_TP_POL_TP1, 1);
    set_key(UBX_CFG_TP_TIMEGRID_TP1, 0);
    set_key(UBX_CFG_TP_ANT_CABLEDELAY, 50);
    rx_used = 0;
    last_layers = 0;
    flash_writes = 0;
    pps_utc = start_unix_seconds - 1;
    pps_edge_utc = 0;
    pps_edge_leap = false;
    leap_change = 0;
    memset(&pps, 0, sizeof(pps));
    drop_next_pps = false;
    epoch_pending = false;
    epoch_counter = 0;
    baud_switch_pending = false;
}


static uint32_t value_of(uint32_t key)
{
    KeyValue *kv = find(key);
    return kv ? kv->value : 0;
}


static void civil_from_unix(int64_t t, UbxNavTimeutc *u)
{
    int64_t days = t / 86400, rem = t % 86400;
    // Hinnant civil_from_days
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    u->year = (uint16_t)(y + (m <= 2));
    u->month = (uint8_t)m;
    u->day = (uint8_t)d;
    u->hour = (uint8_t)(rem / 3600);
    u->minute = (uint8_t)(rem % 3600 / 60);
    u->second = (uint8_t)(rem % 60);
}


// UTC label of continuous second t: an inserted second repeats E-1 as
// 23:59:60, a deleted one skips E-1.
static int64_t label_of(int64_t t, bool *leap)
{
    *leap = false;
    if (leap_change > 0 && t >= leap_event)
    {
        *leap = t == leap_event;
        return t - 1;
    }
    if (leap_change < 0 && t >= leap_event - 1)
    {
        return t + 1;
    }
    return t;
}


static void emit_epoch(void)
{
    uint8_t p[32];
    uint32_t tow = (uint32_t)((pps_utc + 18) % 604800) * 1000U;
    UbxNavTimeutc u;
    bool leap;
    int64_t label = label_of(pps_utc, &leap);
    civil_from_unix(label, &u);
    if (leap)
        u.second = 60;

    if (value_of(UBX_CFG_MSGOUT_NAV_POSLLH_UART1))
    {
        memset(p, 0, 28);
        put_u32(p, tow);
        put_u32(p + 4, (uint32_t)-8400000);
        put_u32(p + 8, 516600000);
        emit(UBX_CLASS_NAV, UBX_ID_NAV_POSLLH, p, 28);
    }
    if (value_of(UBX_CFG_MSGOUT_NAV_STATUS_UART1))
    {
        memset(p, 0, 16);
        put_u32(p, tow);
        p[4] = cfg.locked ? UBX_FIX_3D : UBX_FIX_NONE;
        p[5] = cfg.locked ? 0x0D : 0;
        emit(UBX_CLASS_NAV, UBX_ID_NAV_STATUS, p, 16);
    }
    if (value_of(UBX_CFG_MSGOUT_NAV_TIMEUTC_UART1))
    {
        memset(p, 0, 20);
        put_u32(p, tow);
        put_u32(p + 4, 20);
        put_u32(p + 8, (uint32_t)-150);
        put_u16(p + 12, u.year);
        p[14] = u.month;
        p[15] = u.day;
        p[16] = u.hour;
        p[17] = u.minute;
        p[18] = u.second;
        p[19] = cfg.locked ? 0x37 : 0x00;
        emit(UBX_CLASS_NAV, UBX_ID_NAV_TIMEUTC, p, 20);
    }
    if (value_of(UBX_CFG_MSGOUT_NAV_CLOCK_UART1))
    {
        memset(p, 0, 20);
        put_u32(p, tow);
        put_u32(p + 4, 12345);
        emit(UBX_CLASS_NAV, UBX_ID_NAV_CLOCK, p, 20);
    }
    uint32_t ls_rate = value_of(UBX_CFG_MSGOUT_NAV_TIMELS_UART1);
    if (ls_rate && epoch_counter % ls_rate == 0)
    {
        memset(p, 0, 24);
        put_u32(p, tow);
        p[9] = 18;
        p[11] = 1;
        put_u32(p + 12, 3600);
        if (leap_change != 0)
        {
            // The scheduled event: counts down to it, then up from it.
            bool passed = pps_utc >= leap_event;
            p[9] = (uint8_t)(18 + (passed ? leap_change : 0));
            p[11] = (uint8_t)leap_change;
            put_u32(p + 12, (uint32_t)(int32_t)(leap_event - pps_utc));
        }
        p[23] = 0x03;
        emit(UBX_CLASS_NAV, UBX_ID_NAV_TIMELS, p, 24);
    }
    // NAV-EOE closes the navigation messages of this epoch.
    if (value_of(UBX_CFG_MSGOUT_NAV_EOE_UART1))
    {
        put_u32(p, tow);
        emit(UBX_CLASS_NAV, UBX_ID_NAV_EOE, p, 4);
    }
    /* TIM-TM2: the clock's epoch output on EXTINT, inverted on the board, so
     * the receiver's falling edge is the epoch (this second, offset by the
     * clock's phase) and its rising edge the end of the 100 ms pulse. UTC
     * time base. */
    if (value_of(UBX_CFG_MSGOUT_TIM_TM2_UART1) && cfg.supports_tim_tm2)
    {
        int64_t second_ns = (pps_utc % 604800) * 1000000000LL;
        int64_t falling_ns = second_ns + epoch_mark_offset_ns;
        int64_t rising_ns = second_ns + 100000000LL + 2661;
        memset(p, 0, 28);
        p[0] = 0;
        p[1] = (uint8_t)(UBX_TM2_MODE_RUNNING | (UBX_TIMEBASE_UTC << UBX_TM2_TIMEBASE_SHIFT) |
                         UBX_TM2_UTC_AVAILABLE | (cfg.locked ? UBX_TM2_TIME_VALID : 0U) |
                         UBX_TM2_NEW_RISING_EDGE | UBX_TM2_NEW_FALLING_EDGE);
        put_u16(p + 2, (uint16_t)epoch_counter);
        put_u16(p + 4, 2440);
        put_u16(p + 6, 2440);
        put_u32(p + 8, (uint32_t)(rising_ns / 1000000));
        put_u32(p + 12, (uint32_t)(rising_ns % 1000000));
        put_u32(p + 16, (uint32_t)(falling_ns / 1000000));
        put_u32(p + 20, (uint32_t)(falling_ns % 1000000));
        put_u32(p + 24, 45);
        emit(UBX_CLASS_TIM, UBX_ID_TIM_TM2, p, 28);
    }
    // TIM-TP: the next pulse, sent with this epoch's solution. GNSS time
    // base, UTC available, qErr valid while locked.
    if (value_of(UBX_CFG_MSGOUT_TIM_TP_UART1))
    {
        memset(p, 0, 16);
        put_u32(p, (tow + 1000U) % 604800000U);
        put_u32(p + 4, 0);
        put_u32(p + 8, (uint32_t)qerr_for(pps_utc + 1));
        put_u16(p + 12, 2440);
        p[14] =
            (uint8_t)(UBX_TP_FLAGS_UTC_AVAILABLE | (cfg.locked ? 0U : UBX_TP_FLAGS_QERR_INVALID));
        emit(UBX_CLASS_TIM, UBX_ID_TIM_TP, p, 16);
    }
    epoch_counter++;
}


void sim_step(uint32_t now_ms)
{
    uint8_t tx[1024];
    uint16_t n = host_uart2_take_tx(tx, sizeof(tx));

    if (!cfg.present)
        return;
    if (n > 0 && host_uart2_baud() == baud)
        for (uint16_t i = 0; i < n; i++)
            receiver_feed(tx[i]);
    if (baud_switch_pending)
    {
        baud = pending_baud_switch;
        baud_switch_pending = false;
    }

    if (now_ms % 1000U == 0U)
    {
        pps_utc++;
        if (cfg.locked && !drop_next_pps)
        {
            pps.sequence++;
            pps.observed_ms = now_ms;
            pps.valid = true;
            pps_edge_utc = label_of(pps_utc, &pps_edge_leap);
            last_pps_qerr_ps = qerr_for(pps_utc);
        }
        drop_next_pps = false;
        epoch_pending = true;
        pending_epoch_ms = now_ms + cfg.message_delay_ms;
    }
    if (epoch_pending && now_ms == pending_epoch_ms)
    {
        epoch_pending = false;
        emit_epoch();
    }
}


GnssPpsReference sim_pps_reference(void)
{
    return pps;
}


int64_t sim_utc_of_last_pps(void)
{
    return pps_edge_utc;
}


bool sim_leap_of_last_pps(void)
{
    return pps_edge_leap;
}


void sim_schedule_leap(int64_t event_utc, int8_t change)
{
    leap_event = event_utc;
    leap_change = change;
}


uint32_t sim_baud(void)
{
    return baud;
}


uint32_t sim_config_value(uint32_t key, bool *found)
{
    KeyValue *kv = find(key);
    if (found)
        *found = kv != NULL;
    return kv ? kv->value : 0;
}


uint8_t sim_last_layers(void)
{
    return last_layers;
}


uint32_t sim_flash_writes(void)
{
    return flash_writes;
}


void sim_set_locked(bool locked)
{
    cfg.locked = locked;
}


void sim_drop_next_pps(void)
{
    drop_next_pps = true;
}


int32_t sim_qerr_of_last_pps(void)
{
    return last_pps_qerr_ps;
}


void sim_set_epoch_mark_offset(int32_t offset_ns)
{
    epoch_mark_offset_ns = offset_ns;
}


void sim_set_message_rate(uint32_t key, uint32_t rate)
{
    set_key(key, rate);
}
