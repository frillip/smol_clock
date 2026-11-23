#include "timing_report.h"

#include <stdbool.h>
#include <stdio.h>
#include "../platform/format.h" // our printf (format.h: the library's is slow)

#include "../console/text.h"
#include "timing_config.h"

#define NS_PER_TICK (1000000000UL / TIMING_NOMINAL_TICKS) // 25

// Ticks as microseconds with three decimals (25 ns resolution).
static void text_phase_us(TextLine *line, int32_t phase_ticks)
{
    bool negative = phase_ticks < 0;
    uint32_t magnitude = negative ? (uint32_t)0 - (uint32_t)phase_ticks : (uint32_t)phase_ticks;

    if (negative)
    {
        text_char(line, '-');
    }
    text_u32(line, magnitude / TIMING_TICKS_PER_US, 0, ' ');
    text_char(line, '.');
    text_u32(line, (magnitude % TIMING_TICKS_PER_US) * NS_PER_TICK, 3, '0');
    text_str(line, "us");
}


// Q16 ticks with three decimals, rounded.
static void text_q16_ticks(TextLine *line, int32_t value_q16)
{
    bool negative = value_q16 < 0;
    uint32_t magnitude = negative ? (uint32_t)0 - (uint32_t)value_q16 : (uint32_t)value_q16;
    uint32_t whole = magnitude >> 16;
    uint32_t milli = ((magnitude & 0xFFFFUL) * 1000UL + 32768UL) >> 16;

    if (milli >= 1000U)
    {
        whole++;
        milli -= 1000U;
    }
    if (negative)
    {
        text_char(line, '-');
    }
    text_u32(line, whole, 0, ' ');
    text_char(line, '.');
    text_u32(line, milli, 3, '0');
}


void timing_report_sync(const PpsCaptureSync *sync, bool detail)
{
    uint8_t attempt;

    if (!sync->valid)
    {
        printf("*** IC TIMEBASE SYNC FAILED ***\r\n");
    }
    if (!detail)
    {
        return;
    }
    printf("GPIO IC12=%10lu IC34=%10lu, %u attempt%s, offsets", (unsigned long)sync->ic12_stamp,
           (unsigned long)sync->ic34_stamp, (unsigned)sync->attempts,
           sync->attempts == 1U ? "" : "s");
    for (attempt = 0; attempt < sync->attempts; attempt++)
    {
        if (sync->offsets[attempt] == INT32_MAX)
        {
            printf(" missed");
        }
        else
        {
            printf(" %+ld", (long)sync->offsets[attempt]);
        }
    }
    printf("\r\n");
    if (!sync->valid)
    {
        return;
    }
    if (sync->correction != 0)
    {
        printf("--- IC TIMEBASE SYNC CORRECTED BY %+ld ---\r\n", (long)sync->correction);
    }
    else
    {
        printf("--- IC TIMEBASE SYNC VALID ---\r\n");
    }
}


// ROLL: the cascade erratum fix was applied to that capture (its lower word
// was exactly 0x0000), not anything to do with the GNSS fix.
static void report_measurement(const TimingSnapshot *snapshot, int32_t utc_offset_ticks)
{
    const EpochControl *loop = epoch_control_state();
    const PpsCaptureSample *capture = &snapshot->capture;
    uint32_t kf_whole_hz;
    uint16_t kf_millihz;
    TextLine line;

    kalman_hz(&loop->pps_kf, &kf_whole_hz, &kf_millihz);

    text_start(&line);
    text_u32(&line, snapshot->timer.sequence, 5, '0');
    text_str(&line, capture->reference_input == PPS_INPUT_ESP ? "  ESP: cap=" : " GNSS: cap=");
    text_u32(&line, capture->reference_capture, 10, ' ');
    text_str(&line, " IC12=");
    text_hex16(&line, capture->reference_high);
    text_char(&line, ':');
    text_hex16(&line, capture->reference_low);
    text_str(&line, " ROLL:");
    text_u32(&line, capture->reference_rollover_corrected ? 1U : 0U, 0, ' ');
    text_str(&line, " V:");
    text_u32(&line, capture->reference_capture_valid ? 1U : 0U, 0, ' ');
    text_str(&line, " RAW:");
    text_u32(&line, capture->reference_period, 0, ' ');
    text_str(&line, "Hz KF:");
    text_u32(&line, kf_whole_hz, 0, ' ');
    text_char(&line, '.');
    text_u32(&line, kf_millihz, 3, '0');
    text_str(&line, "Hz\r\n");
    text_send(&line);

    text_str(&line, "      OC: cap=");
    text_u32(&line, capture->epoch_capture, 10, ' ');
    text_str(&line, " IC34=");
    text_hex16(&line, capture->epoch_high);
    text_char(&line, ':');
    text_hex16(&line, capture->epoch_low);
    text_str(&line, " ROLL:");
    text_u32(&line, capture->epoch_rollover_corrected ? 1U : 0U, 0, ' ');
    text_str(&line, " V:");
    text_u32(&line, capture->epoch_capture_valid ? 1U : 0U, 0, ' ');
    text_str(&line, " UTC:");
    text_i32(&line, utc_offset_ticks, 4, false);
    text_char(&line, ' ');
    text_phase_us(&line, utc_offset_ticks);
    text_str(&line, " STATE:");
    text_str(&line, epoch_control_state_name(loop->state));
    text_str(&line, " SEQ:");
    text_u32(&line, snapshot->timer.sequence - loop->last_resync, 0, ' ');
    text_str(&line, " GATED:");
    text_u32(&line, loop->gate.rejected, 0, ' ');
    text_str(&line, " PULSE:");
    text_u32(&line, snapshot->output.pulses_seen, 0, ' ');
    text_str(&line, " MISSED:");
    text_u32(&line, snapshot->output.pulses_missed, 0, ' ');
    text_str(&line, " LATE:");
    text_u32(&line, snapshot->timer.handler_late, 0, ' ');
    text_str(&line, "\r\n");
    text_send(&line);

    text_str(&line, "      PI: EN:");
    text_u32(&line, loop->pi.enabled ? 1U : 0U, 0, ' ');
    text_str(&line, " PRED:");
    text_q16_ticks(&line, loop->predicted_phase_q16);
    text_str(&line, " P:");
    text_q16_ticks(&line, loop->pi.proportional_q16);
    text_str(&line, " I:");
    text_q16_ticks(&line, loop->pi.integral_q16);
    text_str(&line, " OUT:");
    text_q16_ticks(&line, loop->pi.correction_q16);
    text_str(&line, " CMD:");
    text_u32(&line, TIMING_NOMINAL_TICKS, 0, ' ');
    text_char(&line, '+');
    text_q16_ticks(&line, loop->commanded_offset_q16);
    text_str(&line, " NEXT:");
    text_u32(&line, loop->commanded_period, 0, ' ');
    text_str(&line, " FRAC:");
    text_u32(&line, loop->fractional.accumulator_q16, 0, ' ');
    text_str(&line, "\r\n");
    text_send(&line);
}


void timing_report_epoch(const TimingSnapshot *snapshot, const EpochUpdate *update)
{
    const EpochControl *loop = epoch_control_state();
    TextLine line;

    text_start(&line);
    switch (update->kind)
    {
        case EPOCH_UPDATE_MEASURED: report_measurement(snapshot, update->utc_offset_ticks); break;

        case EPOCH_UPDATE_HOLDOVER:
        case EPOCH_UPDATE_FREE_RUN:
            // Not steering: the measured phase, if any, is for information.
            text_u32(&line, snapshot->timer.sequence, 5, '0');
            if (update->kind == EPOCH_UPDATE_HOLDOVER)
            {
                text_str(&line, " holdover ");
                text_u32(&line, loop->holdover_epochs, 0, ' ');
            }
            else
            {
                text_str(&line, " free run ");
                text_str(&line, epoch_control_state_name(loop->state));
            }
            text_char(&line, ':');
            if (update->phase_valid)
            {
                text_str(&line, " UTC:");
                text_i32(&line, update->utc_offset_ticks, 0, false);
                text_char(&line, ' ');
                text_phase_us(&line, update->utc_offset_ticks);
            }
            text_str(&line, " CMD:");
            text_u32(&line, TIMING_NOMINAL_TICKS, 0, ' ');
            text_char(&line, '+');
            text_q16_ticks(&line, loop->commanded_offset_q16);
            text_str(&line, " NEXT:");
            text_u32(&line, loop->commanded_period, 0, ' ');
            text_str(&line, "\r\n");
            text_send(&line);
            break;

        case EPOCH_UPDATE_INCOMPLETE:
        default:
            // The probe is from the previous epoch's pulse (see
            // EpochOutputSample for what a healthy one looks like).
            text_u32(&line, snapshot->timer.sequence, 5, '0');
            text_str(&line, " incomplete capture pair: REF=");
            text_u32(&line, snapshot->capture.reference_capture_valid ? 1U : 0U, 0, ' ');
            text_str(&line, " OC=");
            text_u32(&line, snapshot->capture.epoch_capture_valid ? 1U : 0U, 0, ' ');
            text_str(&line, " OC1 CON1=");
            text_hex16(&line, snapshot->output.probe_con1);
            text_str(&line, " CON2=");
            text_hex16(&line, snapshot->output.probe_con2);
            text_str(&line, " PIN=");
            text_u32(&line, snapshot->output.probe_pin, 0, ' ');
            text_str(&line, " PULSE:");
            text_u32(&line, snapshot->output.pulses_seen, 0, ' ');
            text_str(&line, " MISSED:");
            text_u32(&line, snapshot->output.pulses_missed, 0, ' ');
            text_str(&line, "\r\n");
            text_send(&line);
            break;
    }
}
