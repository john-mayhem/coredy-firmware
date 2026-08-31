#ifndef _COREDY_BRIDGE_
#define _COREDY_BRIDGE_

// Glue layer between the TuyaMCU UART protocol (tuya_uart.c) and the
// SmartThings capability handlers (caps_*.c, same "Maya Robot" profile
// already validated in the dummy-device track). Owns the DP<->capability
// mapping, the boot handshake, heartbeat, and the cmd=0x03/0x05 handling.
//
// Target chip: classic ESP32 (confirmed on-hand hardware, 2026-08-31 --
// NOT the S3 originally planned; pins below avoid classic ESP32's strapping
// pins 0/2/5/12/15).

#include <stdbool.h>
#include "st_dev.h"
#include "caps_battery.h"
#include "caps_robotCleanerOperatingState.h"
#include "caps_ns_cleaningMode.h"
#include "caps_ns_suctionLevel.h"
#include "caps_ns_waterLevel.h"
#include "caps_ns_hardwareFault.h"
#include "caps_ns_areaCleaned.h"
#include "caps_ns_consumables.h"
#include "caps_momentary.h"

#ifdef __cplusplus
extern "C" {
#endif

// Robot J1 wiring -- module perspective, confirmed via continuity check
// 2026-08-31: our RX <- robot "RX" pin, our TX -> robot "TX" pin.
// GPIO16/17 chosen as free-and-safe (also happen to be UART2 default pins
// on classic ESP32, though we bind them explicitly either way) -- rewire in
// coredy_bridge.h if the real J1 harness lands on different pins.
// Swapped 2026-08-31 during bench bring-up: the physical solder job landed
// TX/RX reversed vs. this original assignment (zero traffic either direction
// until swapped in firmware) -- same class of mixup as the sniffer's
// GPIO6/7 mislabel earlier this project. Swapping here instead of
// resoldering.
#define COREDY_UART_RX_GPIO 17
#define COREDY_UART_TX_GPIO 16

// Confirmed DP ids (docs/PROTOCOL.md)
#define DP_POWER            1
#define DP_AREA_CLEANED     14
#define DP_ERROR            17
#define DP_STATUS           101
#define DP_CLEAN_MODE       102
#define DP_FAN_SPEED        104
#define DP_DIRECTION        105
#define DP_CLEAN_TIME_MIN   107
#define DP_BATTERY_PCT      108
#define DP_BRUSH_PCT        109
#define DP_ROLLER_PCT       110
#define DP_HEPA_PCT         111
#define DP_ACTIVATE         112
#define DP_LOCATE           116
#define DP_MODEL            118
#define DP_DATA_SAMPLE      119

// Water level. Identified 2026-08-31 -- this was the last unmapped DP and the
// only capability with no DP, and they turned out to be the same thing.
//
// Evidence, from writing values live and reading the MCU's echo:
//   1 -> echoed 1            accepted
//   2 -> echoed 2            accepted
//   3 -> echoed 1            REJECTED (returns the previous value, the exact
//                            rejection signature already documented for DP104)
//   any, while stopped -> 4  idle/unavailable, not a settable level
// So it is a state-gated enum, writable only while the robot is running --
// which is why every observation before this was 4: nothing had ever written
// to it while running.
//
// The valid range is 0/1/2 = low/medium/high: 1 and 2 are confirmed, 3 is
// confirmed invalid, and the stock Tuya app exposed exactly three water
// levels. 0 is inferred from that rather than directly observed, and taking
// waterLevel('low') in the app is itself the confirming test -- an echo of 0
// settles it.
#define DP_WATER_LEVEL      120

// Registers the already-initialized capability data pointers (call once
// from main.c, right after capability_init()), then starts the UART and
// boot handshake. Call coredy_bridge_start_heartbeat() separately once the
// rest of app_main() has finished setting up (matches the dummy app's own
// "everything else first, then start the periodic tasks" ordering).
void coredy_bridge_init(
    caps_battery_data_t *battery,
    caps_robotCleanerOperatingState_data_t *operatingState,
    caps_ns_cleaningMode_data_t *cleaningMode,
    caps_ns_suctionLevel_data_t *suctionLevel,
    caps_ns_waterLevel_data_t *waterLevel,
    caps_ns_hardwareFault_data_t *hardwareFault,
    caps_ns_areaCleaned_data_t *areaCleaned,
    caps_ns_consumables_data_t *consumables,
    caps_momentary_data_t *momentary
);

void coredy_bridge_start_heartbeat(void);

// main.c registers a callback here that performs the same
// st_conn_cleanup()+st_conn_start() dance as the dummy app's own
// BUTTON_LONG_PRESS handler -- coredy_bridge.c deliberately doesn't touch
// IOT_CTX directly, main.c owns that lifecycle.
typedef void (*coredy_reset_cb_t)(void);
void coredy_bridge_set_reset_callback(coredy_reset_cb_t cb);

// Feed STDK's own device-status callback through here so the bridge can
// derive the cmd=0x03 network/icon-state enum (0=pairing, 2=connecting,
// 3=router-connected, 4=cloud-connected) and push it to the STM32 whenever
// it changes -- the STM32 does NOT derive this on its own, we own it.
void coredy_bridge_on_st_status(st_device_status status);

// True only while the cloud connection is settled (ST_DEVICE_STATUS_CLOUD_CONNECTED).
// coredy_ota's polling task checks this before ever opening an HTTPS connection --
// found live 2026-08-31 that firing OTA's own TLS handshake while the SmartThings
// MQTT/TLS session was mid-reconnect caused both to fail (resource contention),
// mirroring the same gate the proven Maya SmartSwitch OTA implementation uses.
bool coredy_bridge_is_cloud_connected(void);

// Command-side hooks -- wire these into each capability's cmd_*_usr_cb so a
// SmartThings command gets translated into the matching cmd=0x06 DP frame.
void coredy_bridge_cmd_set_cleaning_mode(const char *value);   // -> DP102
void coredy_bridge_cmd_set_suction_level(const char *value);   // -> DP104
void coredy_bridge_cmd_set_water_level(const char *value);     // no real DP yet -- stub, logs unknown-capability request
void coredy_bridge_cmd_start(void);                            // -> DP112=1
void coredy_bridge_cmd_pause(void);                             // -> DP112=0
void coredy_bridge_cmd_go_home(void);                            // -> DP102=5
void coredy_bridge_cmd_locate(void);                             // -> DP116=true

// Writes DP120 directly, for mapping its still-unknown valid range. Backs
// GET /dp120?value=N. See coredy_bridge_cmd_set_water_level() for what is
// established so far.
void coredy_bridge_probe_dp120(uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
