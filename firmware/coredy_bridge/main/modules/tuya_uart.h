#ifndef _TUYA_UART_
#define _TUYA_UART_

// Low-level TuyaMCU serial framing for the Coredy R750 bridge. This module
// knows nothing about SmartThings or robot semantics -- it only knows how to
// build/send frames and parse/dispatch received ones. See coredy_bridge.c
// for the actual DP<->capability mapping.
//
// Frame: 55 AA [ver] [cmd] [len_hi] [len_lo] [payload...] [checksum]
// checksum = sum of all preceding bytes mod 256.
//
// Empirically confirmed (Desktop\CoredyR750\PROGRESS.md + 2026-08-31 re-sniff,
// corrected for a tap-label mixup on that resniff -- see project memory):
//   - We play the "module" role. Our own frames always stamp ver=0x00 -- this
//     matches the real WR3's own byte, NOT a generic "protocol version".
//   - The STM32 always stamps ver=0x03 on its frames. We don't validate this,
//     just note it -- no reason to reject a frame over it.
//   - cmd 0x00 heartbeat: direction unconfirmed (see coredy_bridge.c), but we
//     send periodic empty pings and separately ack any we receive.
//   - cmd 0x01 product info: WE send an empty query, STM32 responds with its
//     own product-id JSON. (The module is generic/reusable hardware and
//     doesn't inherently know which product it's attached to.)
//   - cmd 0x02 MCU conf query: WE send empty, STM32 empty-acks.
//   - cmd 0x03 network/icon state: WE send a 1-byte enum
//     (0=pairing/smartconfig, 2=connecting, 3=router-connected,
//     4=cloud-connected) as our own connection state changes; STM32
//     empty-acks. The STM32 drives its own WiFi-icon LED off this value --
//     we MUST push it accurately or the icon lies.
//   - cmd 0x05 network reset: STM32 sends payload=0x00 (fixed) when the
//     physical button is long-pressed; WE ack empty, then must actually
//     perform an onboarding reset (mirrors the real module's observed
//     behavior of replaying its own 0x01/0x02 handshake and announcing
//     state=0 immediately after).
//   - cmd 0x06 command down: WE send this (DP payload) whenever a
//     SmartThings command needs to reach the STM32.
//   - cmd 0x07 status report: STM32 sends this (DP payload, one or more DPs)
//     both spontaneously and after our 0x08 query-all. We never ack it.
//   - cmd 0x08 query-all: WE send empty to request a full DP dump.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TUYA_DP_TYPE_RAW    0x00
#define TUYA_DP_TYPE_BOOL   0x01
#define TUYA_DP_TYPE_VALUE  0x02
#define TUYA_DP_TYPE_STRING 0x03
#define TUYA_DP_TYPE_ENUM   0x04
#define TUYA_DP_TYPE_BITMAP 0x05

typedef struct {
    uint8_t dpid;
    uint8_t dptype;
    uint16_t len;
    const uint8_t *value; // points into the RX frame buffer -- only valid for the duration of the callback
} tuya_dp_t;

// Fired once per DP record found inside a cmd=0x07 status frame.
typedef void (*tuya_dp_cb_t)(const tuya_dp_t *dp);

// Fired for every received frame whose cmd is NOT 0x07 (handshake/ack/reset
// bytes, plus anything unrecognized). `cmd` lets the caller dispatch;
// `payload`/`len` may be zero-length for empty-ack frames.
typedef void (*tuya_cmd_cb_t)(uint8_t cmd, const uint8_t *payload, uint16_t len);

// Starts the UART peripheral and the RX parser task. tx_gpio/rx_gpio are the
// pins wired to the robot's J1 TX/RX pins respectively (module perspective:
// our RX <- robot "RX" pin, our TX -> robot "TX" pin -- confirmed via
// continuity check 2026-08-31, see project memory).
void tuya_uart_init(int tx_gpio, int rx_gpio);

void tuya_uart_set_dp_callback(tuya_dp_cb_t cb);
void tuya_uart_set_cmd_callback(tuya_cmd_cb_t cb);

// Builds and transmits a complete frame (ver is always fixed at 0x00 for us).
void tuya_send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len);

// DP-record builder helpers. Each appends one DP record starting at buf[off]
// and returns the new offset. Caller owns the buffer and flushes it via
// tuya_send_frame(cmd, buf, offset) once all DPs are appended.
uint16_t tuya_dp_append_bool(uint8_t *buf, uint16_t off, uint8_t dpid, bool value);
uint16_t tuya_dp_append_enum(uint8_t *buf, uint16_t off, uint8_t dpid, uint8_t value);
uint16_t tuya_dp_append_value(uint8_t *buf, uint16_t off, uint8_t dpid, int32_t value);

// Convenience one-DP senders for cmd=0x06 (command down to STM32).
void tuya_send_cmd06_bool(uint8_t dpid, bool value);
void tuya_send_cmd06_enum(uint8_t dpid, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
