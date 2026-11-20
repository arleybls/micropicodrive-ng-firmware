// QL select-clock counter and drive mode pill — see ql_probe.h.
//
// Two ways of counting the pulses on MD_SER_CLK, one per chip:
//
//   RP2350 (Pico 2 W): a spare state machine on pio2 pushes the pulse count of
//   every burst. PIO inputs see the pad whatever its function select, so the
//   pin is not re-muxed.
//
//   RP2040 (Pico): pio0 and pio1 are both full (4 state machines each, 32 and
//   31 of 32 instructions), so the clock feeds PWM slice 0 as an edge counter
//   instead (GPIO1 is that slice's B pin) and bursts are split in software.
//   The pin's function select moves from PIO to PWM; the drive's shifter keeps
//   reading it, because PIO inputs ignore the function select. The output
//   enable is forced off so the counter can never drive the line.
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "MicroDriveControl.h"
#include "UserInterfaceExtension.h"
#include "ql_probe.h"
#if PICO_RP2040
#include "hardware/pwm.h"
#else
#include "hardware/pio.h"
#include "ql_probe.pio.h"
#endif

#define PROBE_IDLE_US      5000u     // clock quiet this long ends a burst
#define PROBE_HIST         11        // buckets 1..10 pulses, [0] = more than 10
#define PROBE_LAST         4

extern mdstatus_t mdStatus;          // drive core's select state (read-only peek)

static uint32_t s_bursts;
static uint32_t s_hist[PROBE_HIST];
static uint8_t  s_last[PROBE_LAST];  // newest first; 255 = more than 254
static uint32_t s_selects;
static bool     s_selected;

// Drive number, learned from the QL: selecting drive n is a burst of n pulses
// (measured 2026-11-20: DIR mdv1_ gives 1 then 8, DIR mdv2_ gives 2 then 8; the
// 8 is "deselect all"). So the burst that leaves THIS unit selected has as
// many pulses as this unit's position in the chain. 0 = not known yet.
#define PROBE_SELECT_WINDOW_US 50000u
static uint8_t  s_drive;
static uint32_t s_burst_n, s_burst_us;   // newest burst and when it was recorded
static bool     s_want_drive;            // selected before its burst was recorded

static void learn_drive(uint32_t n) {
    if (n >= 1 && n <= 8) s_drive = (uint8_t)n;
}

static void record_burst(uint32_t n) {
    s_burst_n = n;
    s_burst_us = time_us_32();
    if (s_want_drive) {
        s_want_drive = false;
        learn_drive(n);
    }
    s_bursts++;
    s_hist[(n >= 1 && n < PROBE_HIST) ? n : 0]++;
    for (int i = PROBE_LAST - 1; i > 0; i--) s_last[i] = s_last[i - 1];
    s_last[0] = n > 254 ? 255 : (uint8_t)n;
}

#if PICO_RP2040

static uint     s_slice;
static uint16_t s_cnt_seen, s_cnt_base;
static uint32_t s_change_us;

void ql_probe_init(void) {
    s_slice = pwm_gpio_to_slice_num(MD_SER_CLK);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&cfg, PWM_DIV_B_RISING);
    pwm_config_set_clkdiv(&cfg, 1.0f);
    pwm_init(s_slice, &cfg, true);
}

static void drain_bursts(void) {
    // The drive core claims the pin for PIO during its own start-up, which may
    // come after ql_probe_init(): take it back whenever it is not ours.
    // Function select and the output-enable override go in ONE write, so there
    // is no moment where the pin is on the PWM function with its output live.
    if (gpio_get_function(MD_SER_CLK) != GPIO_FUNC_PWM)
        hw_write_masked(&io_bank0_hw->io[MD_SER_CLK].ctrl,
                        (GPIO_FUNC_PWM << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB) |
                        (GPIO_OVERRIDE_LOW << IO_BANK0_GPIO0_CTRL_OEOVER_LSB),
                        IO_BANK0_GPIO0_CTRL_FUNCSEL_BITS | IO_BANK0_GPIO0_CTRL_OEOVER_BITS);

    uint16_t cnt = pwm_get_counter(s_slice);
    uint32_t now = time_us_32();
    if (cnt != s_cnt_seen) {
        s_cnt_seen = cnt;
        s_change_us = now;
    } else if (cnt != s_cnt_base && now - s_change_us >= PROBE_IDLE_US) {
        record_burst((uint16_t)(cnt - s_cnt_base));
        s_cnt_base = cnt;
    }
}

#else

#define PROBE_PIO          pio2
#define PROBE_SM_HZ        1000000u

static uint s_sm;

void ql_probe_init(void) {
    uint offset = pio_add_program(PROBE_PIO, &ql_burst_program);
    s_sm = pio_claim_unused_sm(PROBE_PIO, true);

    pio_sm_config cfg = ql_burst_program_get_default_config(offset);
    sm_config_set_clkdiv(&cfg, (float)clock_get_hz(clk_sys) / PROBE_SM_HZ);
    sm_config_set_in_pins(&cfg, MD_SER_CLK);
    sm_config_set_jmp_pin(&cfg, MD_SER_CLK);
    // FIFOs NOT joined: joining to RX leaves no TX FIFO, and the put below
    // (which loads the timeout) would block forever.
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_NONE);
    pio_sm_init(PROBE_PIO, s_sm, offset, &cfg);

    // Idle timeout into OSR: the program reloads Y from it and never pulls.
    pio_sm_put_blocking(PROBE_PIO, s_sm, PROBE_IDLE_US * (PROBE_SM_HZ / 1000000u) / 2);
    pio_sm_exec(PROBE_PIO, s_sm, pio_encode_pull(false, true));
    pio_sm_set_enabled(PROBE_PIO, s_sm, true);
}

static void drain_bursts(void) {
    while (!pio_sm_is_rx_fifo_empty(PROBE_PIO, s_sm))
        record_burst(pio_sm_get(PROBE_PIO, s_sm));
}

#endif

void ql_probe_task(void) {
    drain_bursts();
    bool selected = (mdStatus == MDS_SELECTED);
    if (selected && !s_selected) {
        s_selects++;
        // The select is confirmed 10 ms after the burst, so the burst is
        // normally recorded already; if not, take the next one recorded.
        if (s_bursts && time_us_32() - s_burst_us < PROBE_SELECT_WINDOW_US)
            learn_drive(s_burst_n);
        else
            s_want_drive = true;
    }
    if (!selected) s_want_drive = false;
    s_selected = selected;
}

// The pill shows, in order of what is known:
//   "B" (amber)  no deselect burst and no select yet: no QL has been seen, so
//                the unit is on the bench or the QL has not touched a drive.
//                Bench-tested 2026-11-20: over ten minutes with no QL attached
//                and line noise never produced a clean burst, so it stays "B".
//   nothing      a QL has been seen, but it has not selected this unit yet.
//   "1".."8"     (green) the drive number, once the QL has selected this unit.
// The screens draw it with themselves (uiext_pill_set hands them the bitmap);
// this only paints it straight onto the display at the moment it changes, and
// never while the drive is selected: no display traffic during QL access.
#define PILL_GREEN 0x07E0
#define PILL_AMBER 0xFD20
static bool s_pill_on = true;
static char s_pill_set, s_pill_drawn;

bool ql_probe_pill_enabled(void) { return s_pill_on; }
void ql_probe_pill_enable(bool on) { s_pill_on = on; }

void ql_probe_pill(void) {
    ql_probe_task();
    char want = 0;
    if (s_pill_on) {
        if (s_drive)          want = (char)('0' + s_drive);
        else if (!s_hist[8])  want = 'B';
    }
    if (want != s_pill_set) {
        uiext_pill_set(want, want == 'B' ? PILL_AMBER : PILL_GREEN);
        s_pill_set = want;
    }
    if (s_pill_drawn != s_pill_set && !s_selected) {
        uiext_pill_draw();   // draws nothing when the pill was removed: the
        s_pill_drawn = s_pill_set;   // next repaint of the screen clears it
    }
}

#if MPD_QL_PROBE
static bool any_key(void) {
    return gpio_get(UIEXT_BTN_UP) == 0 || gpio_get(UIEXT_BTN_DOWN) == 0 ||
           gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0;
}

// UP/DOWN switch between the summary and the histogram; SELECT or K4 leaves.
void ql_probe_run(void) {
    char l1[26], l2[26], l3[26];
    int page = 0;
    uint32_t shown_bursts = UINT32_MAX, shown_selects = UINT32_MAX;
    bool shown_selected = false, redraw = true;

    while (any_key()) sleep_ms(20);
    while (true) {
        ql_probe_task();
        if (redraw || s_bursts != shown_bursts || s_selects != shown_selects ||
            s_selected != shown_selected) {
            redraw = false;
            shown_bursts = s_bursts;
            shown_selects = s_selects;
            shown_selected = s_selected;
            if (page == 0) {
                snprintf(l1, sizeof(l1), "Bursts %lu  of 8: %lu",
                         (unsigned long)s_bursts, (unsigned long)s_hist[8]);
                snprintf(l2, sizeof(l2), "Sel %lu %s  Drive %c",
                         (unsigned long)s_selects, s_selected ? "on" : "off",
                         s_drive ? (char)('0' + s_drive) : '?');
                snprintf(l3, sizeof(l3), "Last %u %u %u %u",
                         s_last[0], s_last[1], s_last[2], s_last[3]);
            } else if (page == 1) {
                snprintf(l1, sizeof(l1), "1:%lu 2:%lu 3:%lu", (unsigned long)s_hist[1],
                         (unsigned long)s_hist[2], (unsigned long)s_hist[3]);
                snprintf(l2, sizeof(l2), "4:%lu 5:%lu 6:%lu", (unsigned long)s_hist[4],
                         (unsigned long)s_hist[5], (unsigned long)s_hist[6]);
                snprintf(l3, sizeof(l3), "7:%lu 8:%lu", (unsigned long)s_hist[7],
                         (unsigned long)s_hist[8]);
            } else {
                snprintf(l1, sizeof(l1), "9:%lu 10:%lu", (unsigned long)s_hist[9],
                         (unsigned long)s_hist[10]);
                snprintf(l2, sizeof(l2), "More than 10: %lu", (unsigned long)s_hist[0]);
                l3[0] = '\0';
            }
            uiext_ota_screen("QL Probe", l1, l2, l3);
        }
        if (gpio_get(UIEXT_BTN_UP) == 0 || gpio_get(UIEXT_BTN_DOWN) == 0) {
            page = (page + (gpio_get(UIEXT_BTN_DOWN) == 0 ? 1 : 2)) % 3;
            while (any_key()) sleep_ms(20);
            redraw = true;
        } else if (gpio_get(UIEXT_BTN_SELECT) == 0 || gpio_get(UIEXT_BTN_CONFIG) == 0) {
            while (any_key()) sleep_ms(20);
            sleep_ms(150);
            return;
        }
        sleep_ms(10);
    }
}
#endif  // MPD_QL_PROBE
