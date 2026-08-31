#ifndef _DIAG_H_
#define _DIAG_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Boot/crash diagnostics that survive a soft reset, plus a JSON snapshot of
// them for GET /status.
//
// Why this is its own module now: the counters lived as file-static RTC vars in
// main.c and were only ever printed in the boot banner. With the ESP32 sealed
// inside the robot, nobody is attached to the log stream at boot, so the single
// most useful question -- "did it reboot, and was it a brownout or a power
// cycle?" -- was unanswerable after the fact. It happened for real on
// 2026-08-31: the device restarted between two log sessions and there was no
// way to tell why. Exposing this over HTTP makes it answerable at any time.

void diag_init(void);              // call first in app_main, before anything else logs

uint32_t diag_boot_count(void);
const char *diag_reset_reason_str(void);
bool diag_last_boot_was_crash(void);

// Renders the full device status document. Returns bytes written, 0 if it
// did not fit.
size_t diag_render_status_json(char *out, size_t out_size);

#endif
