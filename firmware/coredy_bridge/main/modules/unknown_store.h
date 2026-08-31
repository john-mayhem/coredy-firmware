#ifndef _UNKNOWN_STORE_H_
#define _UNKNOWN_STORE_H_

#include <stdint.h>
#include <stddef.h>

// Durable record of every TuyaMCU frame the bridge did not recognise.
//
// Purpose (the whole reason the log server exists): when the STM32 emits a DP
// or command we've never characterised, we need it to reach the workstation so
// the DP map can be extended and new firmware published. Before this module
// that only happened via the SSE log stream -- which means an unknown frame
// seen while no browser happened to be attached was lost forever. With the
// ESP32 sealed inside the reassembled robot that is the normal case, so the
// record has to survive until something asks for it.
//
// Entries are deduplicated on the full (kind, cmd, dpid, dptype, value) tuple
// and carry a hit count plus first/last-seen timestamps, so a DP that fires
// constantly costs one slot rather than flooding the table. Served as JSON
// from GET /unknown for the workstation to poll.
//
// Deliberately RAM-only, not NVS: the poller runs far more often than the
// device reboots, and writing flash on every unrecognised frame would burn
// NVS for data that is only interesting while we're actively extending the
// map. Losing the table on reboot is acceptable; wearing out flash is not.

#define UNKNOWN_STORE_MAX_ENTRIES 32
#define UNKNOWN_STORE_MAX_VALUE   16

typedef enum {
    UNKNOWN_KIND_DP = 0,   // unrecognised DP id inside a cmd=0x07 report
    UNKNOWN_KIND_CMD = 1,  // unrecognised top-level command byte
} unknown_kind_t;

void unknown_store_init(void);

// Both are safe to call from any task.
void unknown_store_record_dp(uint8_t dpid, uint8_t dptype, const uint8_t *value, uint16_t len);
void unknown_store_record_cmd(uint8_t cmd, const uint8_t *payload, uint16_t len);

// Renders the whole table as a JSON object into `out`. Returns the number of
// bytes written (excluding the terminator), or 0 if it did not fit.
size_t unknown_store_render_json(char *out, size_t out_size);

void unknown_store_clear(void);

#endif
