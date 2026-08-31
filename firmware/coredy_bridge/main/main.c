// Coredy R750 SmartThings bridge -- replaces the Tuya WR3 module entirely.
// Speaks TuyaMCU UART to the real STM32 on one side (modules/tuya_uart.c,
// modules/coredy_bridge.c) and SmartThings Direct Connected Device on the
// other, using the exact "Maya Robot" capability profile already validated
// on the dummy device (Desktop\CoredyR750\SMARTTHINGS_DUMMY_DCD.md).
//
// Target chip: classic ESP32 (confirmed hardware on hand 2026-08-31, not the
// S3 originally planned -- see project memory).
//
// Unlike the dummy app this is based on, there is no physical button/LED on
// this board for onboarding-confirm or network-reset: ownership confirm is
// auto-accepted (headless device, no UI to press), and network reset is
// driven by the STM32 itself over UART (cmd=0x05, fired on the robot's own
// physical button long-press) via coredy_bridge's reset callback. A
// power-cycle factory-reset gesture (a la the Maya SmartSwitch project) was
// deliberately NOT added -- the robot has its own hard power switch that
// fully removes power from the ESP32, which is already sufficient recovery.
//
// Structure/conventions (modules/ layout, ESP_LOG over printf, PROJECT_VER
// via the app descriptor, RTC crash diagnostics, log_buffer+http_server for
// remote log viewing) deliberately mirror the Maya SmartSwitch project's
// codebase, adopted wholesale as this project's standard 2026-08-31.

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "st_dev.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"

// coredy_bridge.h pulls in every caps_*.h transitively -- NOT re-included
// directly here on purpose: caps_battery.h/caps_robotCleanerOperatingState.h
// (the stock-capability headers, unlike the custom caps_ns_*.h ones) have no
// include guards and double-including them elsewhere causes a redefinition
// error.
#include "modules/coredy_bridge.h"
#include "modules/coredy_ota.h"
#include "modules/log_buffer.h"
#include "modules/wifi_events.h"
#include "modules/caps_lock.h"
#include "modules/unknown_store.h"
#include "modules/diag.h"

static const char *TAG = "COREDY_MAIN";

// Boot/crash diagnostics moved to modules/diag.c so they can also be served
// from GET /status -- on a sealed device the boot banner alone is useless,
// since nobody is attached to the log stream when it matters.

// onboarding_config_start is null-terminated string
extern const uint8_t onboarding_config_start[] asm("_binary_onboarding_config_json_start");
extern const uint8_t onboarding_config_end[]   asm("_binary_onboarding_config_json_end");

// device_info_start is null-terminated string
extern const uint8_t device_info_start[] asm("_binary_device_info_json_start");
extern const uint8_t device_info_end[]   asm("_binary_device_info_json_end");

IOT_CTX *iot_ctx = NULL;

// WiFi/Network tracking (shared with modules/wifi_events.c -- not static)
uint32_t wifi_disconnect_count = 0;
bool wifi_connected = false;

static caps_battery_data_t *cap_battery_data;
static caps_robotCleanerOperatingState_data_t *cap_operatingState_data;
static caps_ns_cleaningMode_data_t *cap_cleaningMode_data;
static caps_ns_suctionLevel_data_t *cap_suctionLevel_data;
static caps_ns_waterLevel_data_t *cap_waterLevel_data;
static caps_ns_hardwareFault_data_t *cap_hardwareFault_data;
static caps_ns_areaCleaned_data_t *cap_areaCleaned_data;
static caps_ns_consumables_data_t *cap_consumables_data;
static caps_momentary_data_t *cap_momentary_data;

static void on_operatingState_start(caps_robotCleanerOperatingState_data_t *caps_data)
{
    (void)caps_data;
    coredy_bridge_cmd_start();
}

static void on_operatingState_pause(caps_robotCleanerOperatingState_data_t *caps_data)
{
    (void)caps_data;
    coredy_bridge_cmd_pause();
}

static void on_operatingState_goHome(caps_robotCleanerOperatingState_data_t *caps_data)
{
    (void)caps_data;
    coredy_bridge_cmd_go_home();
}

static void on_cleaningMode_cmd(caps_ns_cleaningMode_data_t *caps_data)
{
    coredy_bridge_cmd_set_cleaning_mode(caps_data->cleaningMode_value);
}

static void on_suctionLevel_cmd(caps_ns_suctionLevel_data_t *caps_data)
{
    coredy_bridge_cmd_set_suction_level(caps_data->suctionLevel_value);
}

static void on_waterLevel_cmd(caps_ns_waterLevel_data_t *caps_data)
{
    coredy_bridge_cmd_set_water_level(caps_data->waterLevel_value);
}

static void on_momentary_push(caps_momentary_data_t *caps_data)
{
    (void)caps_data;
    coredy_bridge_cmd_locate();
}

static void capability_init(void)
{
    cap_battery_data = caps_battery_initialize(iot_ctx, "main", NULL, NULL);
    if (cap_battery_data) {
        cap_battery_data->set_battery_value(cap_battery_data, 0);
        cap_battery_data->set_battery_unit(cap_battery_data, caps_helper_battery.attr_battery.unit_percent);
    }

    cap_operatingState_data = caps_robotCleanerOperatingState_initialize(iot_ctx, "main", NULL, NULL);
    if (cap_operatingState_data) {
        cap_operatingState_data->set_operatingState_value(cap_operatingState_data,
            caps_helper_robotCleanerOperatingState.attr_operatingState.value_stopped);
        cap_operatingState_data->cmd_start_usr_cb = on_operatingState_start;
        cap_operatingState_data->cmd_pause_usr_cb = on_operatingState_pause;
        cap_operatingState_data->cmd_goHome_usr_cb = on_operatingState_goHome;
    }

    cap_cleaningMode_data = caps_ns_cleaningMode_initialize(iot_ctx, "main", NULL, NULL);
    if (cap_cleaningMode_data) {
        cap_cleaningMode_data->set_cleaningMode_value(cap_cleaningMode_data, "auto");
        cap_cleaningMode_data->cmd_setCleaningMode_usr_cb = on_cleaningMode_cmd;
    }

    cap_suctionLevel_data = caps_ns_suctionLevel_initialize(iot_ctx, "main", NULL, NULL);
    if (cap_suctionLevel_data) {
        cap_suctionLevel_data->set_suctionLevel_value(cap_suctionLevel_data, "normal");
        cap_suctionLevel_data->cmd_setSuctionLevel_usr_cb = on_suctionLevel_cmd;
    }

    cap_waterLevel_data = caps_ns_waterLevel_initialize(iot_ctx, "main", NULL, NULL);
    if (cap_waterLevel_data) {
        cap_waterLevel_data->set_waterLevel_value(cap_waterLevel_data, "medium");
        cap_waterLevel_data->cmd_setWaterLevel_usr_cb = on_waterLevel_cmd;
    }

    cap_hardwareFault_data = caps_ns_hardwareFault_initialize(iot_ctx, "main", NULL, NULL);
    if (cap_hardwareFault_data) {
        cap_hardwareFault_data->set_summary_value(cap_hardwareFault_data, "None");
    }

    cap_areaCleaned_data = caps_ns_areaCleaned_initialize(iot_ctx, "main", NULL, NULL);
    if (cap_areaCleaned_data) {
        cap_areaCleaned_data->set_area_value(cap_areaCleaned_data, 0);
    }

    cap_consumables_data = caps_ns_consumables_initialize(iot_ctx, "main", NULL, NULL);
    if (cap_consumables_data) {
        cap_consumables_data->set_brush_value(cap_consumables_data, 0);
        cap_consumables_data->set_rollerBrush_value(cap_consumables_data, 0);
        cap_consumables_data->set_filter_value(cap_consumables_data, 0);
    }

    cap_momentary_data = caps_momentary_initialize(iot_ctx, "main", NULL, NULL);
    if (cap_momentary_data) {
        cap_momentary_data->cmd_push_usr_cb = on_momentary_push;
    }
}

static void iot_status_cb(st_device_status device_status, void *usr_data)
{
    (void)usr_data;
    ESP_LOGI(TAG, "Device status %d", device_status);

    if (device_status == ST_DEVICE_STATUS_ONBOARDING_NEED_CONFIRM) {
        // No physical UI on this board -- headless device, auto-accept.
        st_conn_ownership_confirm(iot_ctx, true);
    }

    // NOTE: cancelling the rollback guard deliberately does NOT happen here.
    // Reaching the SmartThings cloud does not prove this image can still fetch
    // its own updates, and an image that can do the former but not the latter
    // is unrecoverable once the ESP32 is sealed inside the robot. coredy_ota.c
    // marks the image valid only after a real versioninfo.json fetch succeeds.

    coredy_bridge_on_st_status(device_status);
}

static void connection_start(void)
{
    int err = st_conn_start(iot_ctx, (st_status_cb)&iot_status_cb, NULL, NULL);
    if (err) {
        ESP_LOGE(TAG, "fail to start connection. err:%d", err);
    }
}

static void connection_start_task(void *arg)
{
    (void)arg;
    connection_start();
    vTaskDelete(NULL);
}

// Registered with coredy_bridge as the network-reset callback -- fired when
// the STM32 sends cmd=0x05 (robot's physical button long-pressed). Mirrors
// the real WR3's observed behavior: tear down, then re-run onboarding.
static void request_network_reset(void)
{
    st_conn_cleanup(iot_ctx, false);
    xTaskCreate(connection_start_task, "connection_task", 1024 * 3, NULL, 10, NULL);
}

static void iot_noti_cb(iot_noti_data_t *noti_data, void *noti_usr_data)
{
    (void)noti_usr_data;
    ESP_LOGI(TAG, "Notification message received");

    if (noti_data->type == IOT_NOTI_TYPE_DEV_DELETED) {
        ESP_LOGW(TAG, "Device deleted from SmartThings -- wiping credentials and rebooting");
        st_conn_cleanup(iot_ctx, true);
    } else if (noti_data->type == IOT_NOTI_TYPE_RATE_LIMIT) {
        ESP_LOGW(TAG, "Rate limit: remaining=%d, seq=%d",
                 noti_data->raw.rate_limit.remainingTime, noti_data->raw.rate_limit.sequenceNumber);
    }
}

void app_main(void)
{
    // Hook esp_log so subscribers (HTTP /logs) can receive everything we
    // print from here on. UART output is unaffected. Must run before
    // anything else logs, so the very first boot lines are captured too.
    log_buffer_init();
    caps_lock_init();
    unknown_store_init();

    diag_init();

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Coredy R750 bridge -- version %s", app_desc->version);
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    unsigned char *onboarding_config = (unsigned char *)onboarding_config_start;
    unsigned int onboarding_config_len = onboarding_config_end - onboarding_config_start;
    unsigned char *device_info = (unsigned char *)device_info_start;
    unsigned int device_info_len = device_info_end - device_info_start;

    iot_ctx = st_conn_init(onboarding_config, onboarding_config_len, device_info, device_info_len);
    if (iot_ctx == NULL) {
        ESP_LOGE(TAG, "fail to create the iot_context -- nothing else can work, stopping here");
        return;
    }
    int iot_err = st_conn_set_noti_cb(iot_ctx, iot_noti_cb, NULL);
    if (iot_err) ESP_LOGE(TAG, "fail to set notification callback function");

    capability_init();

    coredy_bridge_init(cap_battery_data, cap_operatingState_data, cap_cleaningMode_data,
                        cap_suctionLevel_data, cap_waterLevel_data, cap_hardwareFault_data,
                        cap_areaCleaned_data, cap_consumables_data, cap_momentary_data);
    coredy_bridge_set_reset_callback(request_network_reset);
    coredy_bridge_start_heartbeat();
    coredy_ota_start();

    // WiFi/IP event logging + starts the HTTP log server once an IP lands.
    esp_err_t wifi_evt_err = wifi_events_register();
    if (wifi_evt_err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_events_register failed: %s", esp_err_to_name(wifi_evt_err));
    }

    connection_start();
}
