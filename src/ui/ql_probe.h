// QL select-clock counter and drive mode pill.
//
// Counts the clock pulses in each select burst the QL sends. Selecting drive n
// is a burst of n pulses, so the burst that leaves this unit selected gives its
// own position in the chain; that number is shown in a pill at the top-right.
// Observes only: select handling is unchanged.
//
// -DMPD_QL_PROBE=ON adds a System Tools page with the raw burst counts.
#ifndef QL_PROBE_H
#define QL_PROBE_H

#include <stdbool.h>
#include <stdint.h>

// Drive number 1-8, or 0 if not known: the one learned since power-on, else
// the one remembered from before (see config_save_settings).
uint8_t ql_probe_drive(void);
uint8_t ql_probe_drive_saved(void);
bool    ql_probe_drive_needs_save(void);   // learned, not in flash yet, drive idle
void    ql_probe_drive_set_saved(uint8_t n);

void ql_probe_init(void);   // once, on the UI core
void ql_probe_task(void);   // read the counter; call from the main loop
void ql_probe_pill(void);   // ql_probe_task(), then keep the pill up to date.
                            // Call only on the browser and cartridge screens.
bool ql_probe_pill_enabled(void);
void ql_probe_pill_enable(bool on);
#if MPD_QL_PROBE
void ql_probe_run(void);    // System Tools page (blocks until a key is pressed)
#endif

#endif
