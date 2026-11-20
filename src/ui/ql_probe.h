// QL select-clock probe (diagnostic build only, -DMPD_QL_PROBE=ON).
// Counts the clock pulses in each select burst so a "well-formed burst" rule
// can be written from measurements. Observes only: select handling is unchanged.
#ifndef QL_PROBE_H
#define QL_PROBE_H

#include <stdbool.h>

void ql_probe_init(void);   // once, on the UI core
void ql_probe_task(void);   // drain the burst counter; call from the main loop
void ql_probe_pill(void);   // drain, then restamp the corner pill: "B" until a
                            // clean 8-pulse burst is seen, "Q" after, and the
                            // drive number once the QL has selected this unit.
                            // Call only while the display is up.
bool ql_probe_pill_enabled(void);
void ql_probe_pill_enable(bool on);   // not persisted
void ql_probe_run(void);  // System Tools page (blocks until a key is pressed)

#endif
