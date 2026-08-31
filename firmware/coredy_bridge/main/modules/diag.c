#include "diag.h"
#include "coredy_bridge.h"
#include "coredy_ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"

static const char *TAG = "DIAG";

// RTC memory survives soft resets (but not a true power cycle), which is
// exactly the distinction we need: a cleared magic means the power was
// physically removed, a surviving one means the chip reset itself.
RTC_NOINIT_ATTR static uint32_t rtc_boot_count;
RTC_NOINIT_ATTR static uint32_t rtc_crash_magic;
#define RTC_CRASH_MAGIC 0xC0DEDA11

static const char *s_reset_reason = "UNKNOWN";
static esp_reset_reason_t s_reset_code = ESP_RST_UNKNOWN;
static bool s_was_crash = false;

static const char *reset_reason_to_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_EXT:       return "EXTERNAL";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

void diag_init(void)
{
    s_reset_code = esp_reset_reason();
    s_reset_reason = reset_reason_to_str(s_reset_code);

    s_was_crash = (s_reset_code == ESP_RST_PANIC || s_reset_code == ESP_RST_INT_WDT ||
                   s_reset_code == ESP_RST_TASK_WDT || s_reset_code == ESP_RST_WDT ||
                   s_reset_code == ESP_RST_BROWNOUT);

    if (rtc_crash_magic == RTC_CRASH_MAGIC) {
        rtc_boot_count++;
        if (s_was_crash) {
            ESP_LOGE(TAG, "!!! UNCLEAN RESET !!! reason=%s boot #%lu",
                     s_reset_reason, (unsigned long)rtc_boot_count);
        }
    } else {
        rtc_boot_count = 1;
        ESP_LOGI(TAG, "Fresh boot (power cycle or first run)");
    }
    rtc_crash_magic = RTC_CRASH_MAGIC;

    ESP_LOGW(TAG, "Reset reason: %s (%d)", s_reset_reason, s_reset_code);
    ESP_LOGI(TAG, "Boot count: %lu", (unsigned long)rtc_boot_count);
}

uint32_t diag_boot_count(void)        { return rtc_boot_count; }
const char *diag_reset_reason_str(void) { return s_reset_reason; }
bool diag_last_boot_was_crash(void)   { return s_was_crash; }

size_t diag_render_status_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;

    const esp_app_desc_t *app = esp_app_get_description();

    int rssi = 0;
    int channel = 0;
    char ssid[33] = {0};
    bool wifi_ok = false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        wifi_ok = true;
        rssi = ap.rssi;
        channel = ap.primary;
        strncpy(ssid, (const char *)ap.ssid, sizeof(ssid) - 1);
    }

    int n = snprintf(out, out_size,
        "{"
        "\"version\":\"%s\","
        "\"project\":\"%s\","
        "\"uptime_ms\":%lld,"
        "\"boot_count\":%lu,"
        "\"reset_reason\":\"%s\","
        "\"reset_reason_code\":%d,"
        "\"unclean_reset\":%s,"
        "\"heap_free\":%u,"
        "\"heap_largest_block\":%u,"
        "\"heap_min_free_ever\":%u,"
        "\"wifi_connected\":%s,"
        "\"wifi_ssid\":\"%s\","
        "\"wifi_rssi_dbm\":%d,"
        "\"wifi_channel\":%d,"
        "\"cloud_connected\":%s,"
        "\"ota_pending_verify\":%s,"
        "\"ota_fetch_ok\":%s"
        "}",
        app->version,
        app->project_name,
        (long long)(esp_timer_get_time() / 1000),
        (unsigned long)rtc_boot_count,
        s_reset_reason,
        (int)s_reset_code,
        s_was_crash ? "true" : "false",
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)esp_get_minimum_free_heap_size(),
        wifi_ok ? "true" : "false",
        ssid,
        rssi,
        channel,
        coredy_bridge_is_cloud_connected() ? "true" : "false",
        coredy_ota_is_pending_verify() ? "true" : "false",
        coredy_ota_has_fetched_ok() ? "true" : "false");

    if (n < 0 || (size_t)n >= out_size) {
        ESP_LOGW(TAG, "status buffer too small (%u)", (unsigned)out_size);
        return 0;
    }
    return (size_t)n;
}
