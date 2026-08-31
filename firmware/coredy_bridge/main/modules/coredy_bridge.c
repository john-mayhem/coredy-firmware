#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "coredy_bridge.h"
#include "tuya_uart.h"
#include "unknown_store.h"

static const char *TAG = "COREDY_BRIDGE";

// Measured, not guessed: the recon sniffer's heartbeat counter is global
// across both directions, and across a 2100s capture it ticked 20 counts per
// 150.08s (#21 @159.042s, #41 @309.123s, #61 @459.204s) = 7.504s combined =
// 15.0s per side, rock steady. So the real WR3 pings every 15s. We ping every
// 10s -- deliberately more often than the module we replaced, so the STM32's
// link timeout (whatever it is) can never be the thing that trips first.
#define COREDY_HEARTBEAT_MS 10000

static caps_battery_data_t *s_battery;
static caps_robotCleanerOperatingState_data_t *s_operatingState;
static caps_ns_cleaningMode_data_t *s_cleaningMode;
static caps_ns_suctionLevel_data_t *s_suctionLevel;
static caps_ns_waterLevel_data_t *s_waterLevel;
static caps_ns_hardwareFault_data_t *s_hardwareFault;
static caps_ns_areaCleaned_data_t *s_areaCleaned;
static caps_ns_consumables_data_t *s_consumables;
static caps_momentary_data_t *s_momentary;

static int s_last_dp101 = -1;   // status enum, -1 = not yet seen
static int s_last_dp112 = -1;   // activate (start/pause), -1 = not yet seen
static uint32_t s_last_dp17 = 0; // fault bitmap
static int s_last_net_state = -1; // last cmd=0x03 value we pushed, -1 = never pushed
static bool s_did_initial_query_all = false;
static bool s_cloud_connected = false;

// Last value actually published to SmartThings, per attribute. The STM32
// re-reports unchanged DPs constantly -- a few short bench captures held 110
// DP112 records, 82 DP101 and 39 DP17 -- and every publish is a cloud message
// that counts against the rate limit main.c already has a handler for. These
// caches turn "report" into "report only when it changed".
#define PUB_STR_MAX 40
static char s_pub_opstate[PUB_STR_MAX];
static char s_pub_fault[128];
static char s_pub_cleaningMode[PUB_STR_MAX];
static char s_pub_suction[PUB_STR_MAX];
static int  s_pub_battery = -1;
static int  s_pub_area_raw = -1;
static int  s_pub_brush = -1;
static int  s_pub_roller = -1;
static int  s_pub_hepa = -1;

static coredy_reset_cb_t s_reset_cb = NULL;

void coredy_bridge_set_reset_callback(coredy_reset_cb_t cb) { s_reset_cb = cb; }

// Returns true (and updates the cache) only when `value` differs from what we
// last published for this attribute.
static bool changed_str(char *cache, size_t cache_size, const char *value)
{
    if (strncmp(cache, value, cache_size - 1) == 0) return false;
    strncpy(cache, value, cache_size - 1);
    cache[cache_size - 1] = '\0';
    return true;
}

static bool changed_int(int *cache, int value)
{
    if (*cache == value) return false;
    *cache = value;
    return true;
}

// Forget everything we think the cloud knows.
//
// Necessary because the caches above suppress re-publishing: without this, a
// cloud reconnect would re-hydrate our LOCAL state from the STM32 (cmd=0x08)
// and then publish none of it, because every value still matches the cache
// from before the disconnect. Any divergence SmartThings picked up while we
// were away would stay stale forever. Before the caches existed the constant
// unconditional re-publishing masked this by accident.
//
// Called immediately before each query-all, so the burst that answers it
// republishes the full picture.
static void reset_publish_cache(void)
{
    s_pub_opstate[0] = '\0';
    s_pub_fault[0] = '\0';
    s_pub_cleaningMode[0] = '\0';
    s_pub_suction[0] = '\0';
    s_pub_battery = -1;
    s_pub_area_raw = -1;
    s_pub_brush = -1;
    s_pub_roller = -1;
    s_pub_hepa = -1;
}

static uint32_t read_be(const uint8_t *v, uint16_t len)
{
    uint32_t val = 0;
    uint16_t n = len > 4 ? 4 : len;
    for (uint16_t i = 0; i < n; i++) val = (val << 8) | v[i];
    return val;
}

static void hex_render(char *out, size_t out_size, const uint8_t *buf, uint16_t len)
{
    size_t w = 0;
    for (uint16_t i = 0; i < len && w + 4 < out_size; i++) {
        w += snprintf(out + w, out_size - w, "%02X ", buf[i]);
    }
    if (w > 0) out[w - 1] = '\0';
    else out[0] = '\0';
}

// Anything we don't recognise goes to two places: the log (for anyone watching
// live) and unknown_store (which survives until the workstation polls it). The
// store is the one that matters now that the ESP32 lives sealed inside the
// robot -- see unknown_store.h.
static void log_unknown_dp(uint8_t dpid, uint8_t dptype, const uint8_t *val, uint16_t len)
{
    char hex[64];
    hex_render(hex, sizeof(hex), val, len);
    ESP_LOGW(TAG, "UNKNOWN DP#%u type=0x%02X len=%u raw=%s", dpid, dptype, len, hex);
    unknown_store_record_dp(dpid, dptype, val, len);
}

static void log_unknown_cmd(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    char hex[64];
    hex_render(hex, sizeof(hex), payload, len);
    ESP_LOGW(TAG, "UNKNOWN cmd=0x%02X len=%u raw=%s", cmd, len, hex);
    unknown_store_record_cmd(cmd, payload, len);
}

static void handle_fault_bitmap(uint32_t bits)
{
    static const struct { uint32_t bit; const char *name; } faults[] = {
        {1, "cliff"}, {2, "imp"}, {4, "whl"}, {8, "brush"}, {16, "fan"},
        {32, "roller_brush"}, {64, "low_power"}, {128, "give_up"}, {256, "no_dust"},
    };
    char buf[128] = {0};
    bool first = true;
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        if (bits & faults[i].bit) {
            if (!first) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, faults[i].name, sizeof(buf) - strlen(buf) - 1);
            first = false;
        }
    }
    if (buf[0] == '\0') strcpy(buf, bits == 0 ? "None" : "unrecognized");

    if (s_hardwareFault && changed_str(s_pub_fault, sizeof(s_pub_fault), buf)) {
        s_hardwareFault->set_summary_value(s_hardwareFault, buf);
        s_hardwareFault->attr_summary_send(s_hardwareFault);
    }
}

// Combines DP101 (status), DP112 (activate) and DP17 (fault) into the stock
// robotCleanerOperatingState enum. Fault mapping is a best-effort choice of
// the closest stock value per bit, not something independently confirmed
// bit-by-bit against the real app's own equivalent display.
static void update_operating_state(void)
{
    if (!s_operatingState) return;

    const char *value;
    if (s_last_dp17 & (1 | 2 | 4 | 8 | 32)) { // cliff/imp/whl/brush/roller_brush -- physically stuck/obstructed
        value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_stuck;
    } else if (s_last_dp17 & 16) { // fan fault
        value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_unableToCompleteOperation;
    } else if (s_last_dp17 & 64) { // low_power
        value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_unableToStartOrResume;
    } else if (s_last_dp17 & 128) { // give_up
        value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_unableToCompleteOperation;
    } else {
        // bit 256 (no_dust) is informational, not fatal -- falls through to
        // the DP101-derived value below same as no fault at all.
        switch (s_last_dp101) {
            case 0: // standby -- observed live 2026-08-31 (idle, off dock). Not in
                    // the original schema notes, which only listed 1/2/4/5.
                value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_stopped; break;
            case 1: value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_paused; break;
            case 2: value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_running; break;
            case 4: value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_seekingCharger; break;
            case 5: value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_charging; break;
            default:
                if (s_last_dp101 >= 0) {
                    ESP_LOGW(TAG, "DP101=%d is an unconfirmed state (charged/cleaning_complete "
                             "never captured yet) -- falling back to DP112", s_last_dp101);
                }
                // DP112 is the confirmed start/pause signal and is the most
                // frequently reported DP on the bus, so it is the right
                // fallback whenever DP101 is missing or unrecognised.
                if (s_last_dp112 == 1) {
                    value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_running;
                } else if (s_last_dp112 == 0) {
                    value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_paused;
                } else {
                    value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_stopped;
                }
                break;
        }
    }

    if (!changed_str(s_pub_opstate, sizeof(s_pub_opstate), value)) return;

    s_operatingState->set_operatingState_value(s_operatingState, value);
    s_operatingState->attr_operatingState_send(s_operatingState);
}

static void handle_dp102(uint32_t v)
{
    if (v == 6) {
        // The confirmed fault-overload value on this DP -- not a real mode.
        // DP17 (which co-fires in the same burst) already drives
        // hardwareFault/operatingState; nothing more to do with cleaningMode.
        return;
    }
    const char *mode;
    switch (v) {
        case 0: mode = "auto"; break;
        case 2: mode = "edge"; break;
        case 3: mode = "spot"; break;
        case 4: mode = "smallRoom"; break;
        case 5: mode = "home"; break;
        default:
            // v==1 (random) is a real, schema-confirmed value with no slot in
            // this profile's cleaningMode enum (no UI button for it either) --
            // understood, just unrepresentable right now, not a true unknown.
            ESP_LOGI(TAG, "DP102=%lu has no cleaningMode slot in this profile, ignoring", (unsigned long)v);
            return;
    }
    if (s_cleaningMode && changed_str(s_pub_cleaningMode, sizeof(s_pub_cleaningMode), mode)) {
        s_cleaningMode->set_cleaningMode_value(s_cleaningMode, mode);
        s_cleaningMode->attr_cleaningMode_send(s_cleaningMode);
    }
}

static void handle_dp104(uint32_t v)
{
    const char *level;
    switch (v) {
        case 0: level = "quiet"; break;
        case 1: level = "normal"; break;
        case 2: level = "max"; break;
        default:
            ESP_LOGW(TAG, "DP104=%lu out of the confirmed 0-2 range", (unsigned long)v);
            return;
    }
    if (s_suctionLevel && changed_str(s_pub_suction, sizeof(s_pub_suction), level)) {
        s_suctionLevel->set_suctionLevel_value(s_suctionLevel, level);
        s_suctionLevel->attr_suctionLevel_send(s_suctionLevel);
    }
}

static void on_dp(const tuya_dp_t *dp)
{
    uint32_t v = read_be(dp->value, dp->len);

    switch (dp->dpid) {
        case DP_AREA_CLEANED:
            if (s_areaCleaned && changed_int(&s_pub_area_raw, (int)v)) {
                s_areaCleaned->set_area_value(s_areaCleaned, v / 10.0);
                s_areaCleaned->attr_area_send(s_areaCleaned);
            }
            break;
        case DP_ERROR:
            s_last_dp17 = v;
            handle_fault_bitmap(v);
            update_operating_state();
            break;
        case DP_STATUS:
            s_last_dp101 = (int)v;
            update_operating_state();
            break;
        case DP_CLEAN_MODE:
            handle_dp102(v);
            break;
        case DP_FAN_SPEED:
            handle_dp104(v);
            break;
        case DP_BATTERY_PCT:
            if (s_battery && changed_int(&s_pub_battery, (int)v)) {
                s_battery->set_battery_value(s_battery, (int)v);
                s_battery->attr_battery_send(s_battery);
            }
            break;
        // DP109/110/111 arrive as three separate frames. Publish only the one
        // that landed -- attr_send_all() would broadcast the two siblings that
        // are still 0, which was observed live painting zeros into the app.
        case DP_BRUSH_PCT:
            if (s_consumables && changed_int(&s_pub_brush, (int)v)) {
                s_consumables->set_brush_value(s_consumables, (int)v);
                s_consumables->attr_brush_send(s_consumables);
            }
            break;
        case DP_ROLLER_PCT:
            if (s_consumables && changed_int(&s_pub_roller, (int)v)) {
                s_consumables->set_rollerBrush_value(s_consumables, (int)v);
                s_consumables->attr_rollerBrush_send(s_consumables);
            }
            break;
        case DP_HEPA_PCT:
            if (s_consumables && changed_int(&s_pub_hepa, (int)v)) {
                s_consumables->set_filter_value(s_consumables, (int)v);
                s_consumables->attr_filter_send(s_consumables);
            }
            break;
        case DP_ACTIVATE:
            s_last_dp112 = (int)v;
            update_operating_state();
            break;
        case DP_POWER:
        case DP_DIRECTION:
        case DP_CLEAN_TIME_MIN:
        case DP_LOCATE:
        case DP_MODEL:
        case DP_DATA_SAMPLE:
            // Understood DPs with no matching capability in this profile --
            // inert on purpose, not logged as unknown.
            break;
        case DP_UNRESOLVED_120:
            // NOT inert, and NOT static metadata as previously assumed. The
            // recon captures show the app sending DP120=1 and DP120=2 as
            // cmd=0x06 writes while the MCU always answers cmd=0x07 with 4 --
            // i.e. a writable control whose every write is being rejected,
            // the same accept/reject signature already documented for DP104.
            // Route it into the unknown store so the value pattern is
            // recorded for the workstation until we identify it (leading
            // hypothesis: water/mop level, the one confirmed-missing DP).
            log_unknown_dp(dp->dpid, dp->dptype, dp->value, dp->len);
            break;
        default:
            log_unknown_dp(dp->dpid, dp->dptype, dp->value, dp->len);
            break;
    }
}

static void on_cmd(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    switch (cmd) {
        case 0x00: // heartbeat received from STM32 -- do NOT echo it back. Discovered live
                   // 2026-08-31: echoing every 0x00 we receive created a runaway ping-pong
                   // loop with the STM32 (which apparently does the same), flooding the UART
                   // as fast as both sides could process bytes and drowning out real traffic
                   // (this is almost certainly why an app "Start" command produced no visible
                   // effect earlier). We rely solely on our own periodic heartbeat_task ping;
                   // no reply needed here.
            break;
        case 0x01: // STM32's product-info JSON response to our boot-time query
            ESP_LOGI(TAG, "STM32 product info: %.*s", (int)len, (const char *)payload);
            break;
        case 0x02: // ack to our conf/work-mode query -- nothing to do
            break;
        case 0x03: // ack to our own network-state push -- nothing to do
            break;
        case 0x05: // STM32-triggered network reset (button long-press), payload fixed 0x00
            ESP_LOGW(TAG, "STM32 requested network reset (cmd=0x05)");
            tuya_send_frame(0x05, NULL, 0); // ack, mirrors real WR3's observed behavior
            if (s_reset_cb) s_reset_cb();
            break;
        default:
            log_unknown_cmd(cmd, payload, len);
            break;
    }
}

void coredy_bridge_init(
    caps_battery_data_t *battery,
    caps_robotCleanerOperatingState_data_t *operatingState,
    caps_ns_cleaningMode_data_t *cleaningMode,
    caps_ns_suctionLevel_data_t *suctionLevel,
    caps_ns_waterLevel_data_t *waterLevel,
    caps_ns_hardwareFault_data_t *hardwareFault,
    caps_ns_areaCleaned_data_t *areaCleaned,
    caps_ns_consumables_data_t *consumables,
    caps_momentary_data_t *momentary)
{
    s_battery = battery;
    s_operatingState = operatingState;
    s_cleaningMode = cleaningMode;
    s_suctionLevel = suctionLevel;
    s_waterLevel = waterLevel;
    s_hardwareFault = hardwareFault;
    s_areaCleaned = areaCleaned;
    s_consumables = consumables;
    s_momentary = momentary;

    tuya_uart_set_dp_callback(on_dp);
    tuya_uart_set_cmd_callback(on_cmd);
    tuya_uart_init(COREDY_UART_TX_GPIO, COREDY_UART_RX_GPIO);

    // Boot handshake -- best-effort replica of the real WR3's observed
    // startup order (query product info, then conf/work-mode). The STM32
    // answers asynchronously via on_cmd() above.
    tuya_send_frame(0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    tuya_send_frame(0x02, NULL, 0);
    ESP_LOGI(TAG, "bridge initialized, boot handshake sent");
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(COREDY_HEARTBEAT_MS));
        tuya_send_frame(0x00, NULL, 0);
    }
}

void coredy_bridge_start_heartbeat(void)
{
    // 4096, not 2048: ESP_LOGI's vprintf hook (log_buffer.c) puts a 240-byte
    // formatting buffer on this stack on top of tuya_uart.c's own hex_render
    // buffer in the same call chain, and the very first heartbeat's self-log
    // overflowed 2048 the instant WiFi came up.
    xTaskCreate(heartbeat_task, "coredy_heartbeat", 4096, NULL, 5, NULL);
}

bool coredy_bridge_is_cloud_connected(void) { return s_cloud_connected; }

void coredy_bridge_on_st_status(st_device_status status)
{
    s_cloud_connected = (status == ST_DEVICE_STATUS_CLOUD_CONNECTED);

    uint8_t state;
    switch (status) {
        case ST_DEVICE_STATUS_ONBOARDING_READY:
        case ST_DEVICE_STATUS_ONBOARDING_START:
        case ST_DEVICE_STATUS_ONBOARDING_NEED_CONFIRM:
            state = 0x00; // pairing/smartconfig -- confirmed anchor (fired exactly on real pairing entry)
            break;
        case ST_DEVICE_STATUS_ONBOARDING_ONBOARDED:
            state = 0x02; // best-effort: credentials just received, associating -- not independently anchored
            break;
        case ST_DEVICE_STATUS_CLOUD_DISCONNECTED:
            state = 0x03; // best-effort: has network, lost cloud -- not independently anchored
            break;
        case ST_DEVICE_STATUS_CLOUD_CONNECTED:
            state = 0x04; // confirmed anchor
            break;
        case ST_DEVICE_STATUS_INIT:
        default:
            return; // nothing meaningful to report yet
    }

    if ((int)state != s_last_net_state) {
        s_last_net_state = state;
        tuya_send_frame(0x03, &state, 1);
    }

    if (status == ST_DEVICE_STATUS_CLOUD_CONNECTED && !s_did_initial_query_all) {
        s_did_initial_query_all = true;
        reset_publish_cache();          // so the answering burst actually republishes
        tuya_send_frame(0x08, NULL, 0); // hydrate our capability shadow with real STM32 state
    }
    if (status != ST_DEVICE_STATUS_CLOUD_CONNECTED) {
        s_did_initial_query_all = false; // re-hydrate again on the next reconnect
    }
}

void coredy_bridge_cmd_set_cleaning_mode(const char *value)
{
    uint8_t dp;
    if (!strcmp(value, "auto")) dp = 0;
    else if (!strcmp(value, "edge")) dp = 2;
    else if (!strcmp(value, "spot")) dp = 3;
    else if (!strcmp(value, "smallRoom")) dp = 4;
    else if (!strcmp(value, "home")) dp = 5;
    else { ESP_LOGW(TAG, "unknown cleaningMode '%s', not sending", value); return; }
    tuya_send_cmd06_enum(DP_CLEAN_MODE, dp);
}

void coredy_bridge_cmd_set_suction_level(const char *value)
{
    uint8_t dp;
    if (!strcmp(value, "quiet")) dp = 0;
    else if (!strcmp(value, "normal")) dp = 1;
    else if (!strcmp(value, "max")) dp = 2;
    else { ESP_LOGW(TAG, "unknown suctionLevel '%s', not sending", value); return; }
    tuya_send_cmd06_enum(DP_FAN_SPEED, dp);
}

void coredy_bridge_cmd_set_water_level(const char *value)
{
    // No confirmed DP for water level exists yet (Desktop\CoredyR750\PROGRESS.md
    // -- "not found anywhere"). The capability exists for UI completeness only.
    // DP120 is the leading candidate; once the water-tank test confirms it this
    // becomes a real tuya_send_cmd06_enum(DP_UNRESOLVED_120, ...) call.
    ESP_LOGW(TAG, "setWaterLevel('%s') requested but no backing DP exists yet -- ignored", value);
}

void coredy_bridge_cmd_start(void)   { tuya_send_cmd06_enum(DP_ACTIVATE, 1); }
void coredy_bridge_cmd_pause(void)   { tuya_send_cmd06_enum(DP_ACTIVATE, 0); }
void coredy_bridge_cmd_go_home(void) { tuya_send_cmd06_enum(DP_CLEAN_MODE, 5); }
void coredy_bridge_cmd_locate(void)  { tuya_send_cmd06_bool(DP_LOCATE, true); }
