#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "st_dev.h"
#include "esp_log.h"
#include "caps_robotCleanerOperatingState.h"
#include "caps_lock.h"

static const char *TAG = "CAPS_OPSTATE";

static int caps_robotCleanerOperatingState_attr_operatingState_str2idx(const char *value)
{
    int index;

    for (index = 0; index < CAP_ENUM_ROBOTCLEANEROPERATINGSTATE_OPERATINGSTATE_VALUE_MAX; index++) {
        if (!strcmp(value, caps_helper_robotCleanerOperatingState.attr_operatingState.values[index])) {
            return index;
        }
    }
    return -1;
}

static const char *caps_robotCleanerOperatingState_get_operatingState_value(caps_robotCleanerOperatingState_data_t *caps_data)
{
    if (!caps_data) {
        ESP_LOGE(TAG, "caps_data is NULL");
        return NULL;
    }
    return caps_data->operatingState_value;
}

static void caps_robotCleanerOperatingState_set_operatingState_value(caps_robotCleanerOperatingState_data_t *caps_data, const char *value)
{
    if (!caps_data) {
        ESP_LOGE(TAG, "caps_data is NULL");
        return;
    }
    caps_lock_take();
    if (caps_data->operatingState_value) {
        free(caps_data->operatingState_value);
    }
    caps_data->operatingState_value = strdup(value);
    caps_lock_give();
}

static void caps_robotCleanerOperatingState_attr_operatingState_send(caps_robotCleanerOperatingState_data_t *caps_data)
{
    int sequence_no = -1;

    if (!caps_data || !caps_data->handle) {
        ESP_LOGE(TAG, "fail to get handle");
        return;
    }

    // This capability has the most writers of any in the app -- the STM32's
    // DP101/DP17 reports on the tuya_uart_rx task, plus start/pause/goHome
    // taps on STDK's command task. Snapshot under the lock, publish the copy.
    caps_lock_take();
    char *snapshot = caps_data->operatingState_value ? strdup(caps_data->operatingState_value) : NULL;
    caps_lock_give();
    if (!snapshot) {
        ESP_LOGW(TAG, "value is NULL");
        return;
    }

    ST_CAP_SEND_ATTR_STRING(caps_data->handle,
            (char *)caps_helper_robotCleanerOperatingState.attr_operatingState.name,
            snapshot,
            NULL,
            NULL,
            sequence_no);
    free(snapshot);

    if (sequence_no < 0)
        ESP_LOGE(TAG, "fail to send operatingState value");
}


static void caps_robotCleanerOperatingState_cmd_start_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_robotCleanerOperatingState_data_t *caps_data = (caps_robotCleanerOperatingState_data_t *)usr_data;
    const char* value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_running;

    ESP_LOGI(TAG, "start (num_args:%u)", cmd_data->num_args);

    caps_robotCleanerOperatingState_set_operatingState_value(caps_data, value);
    if (caps_data && caps_data->cmd_start_usr_cb)
        caps_data->cmd_start_usr_cb(caps_data);
    caps_robotCleanerOperatingState_attr_operatingState_send(caps_data);
}

static void caps_robotCleanerOperatingState_cmd_pause_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_robotCleanerOperatingState_data_t *caps_data = (caps_robotCleanerOperatingState_data_t *)usr_data;
    const char* value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_paused;

    ESP_LOGI(TAG, "pause (num_args:%u)", cmd_data->num_args);

    caps_robotCleanerOperatingState_set_operatingState_value(caps_data, value);
    if (caps_data && caps_data->cmd_pause_usr_cb)
        caps_data->cmd_pause_usr_cb(caps_data);
    caps_robotCleanerOperatingState_attr_operatingState_send(caps_data);
}

static void caps_robotCleanerOperatingState_cmd_goHome_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_robotCleanerOperatingState_data_t *caps_data = (caps_robotCleanerOperatingState_data_t *)usr_data;
    const char* value = caps_helper_robotCleanerOperatingState.attr_operatingState.value_seekingCharger;

    ESP_LOGI(TAG, "goHome (num_args:%u)", cmd_data->num_args);

    caps_robotCleanerOperatingState_set_operatingState_value(caps_data, value);
    if (caps_data && caps_data->cmd_goHome_usr_cb)
        caps_data->cmd_goHome_usr_cb(caps_data);
    caps_robotCleanerOperatingState_attr_operatingState_send(caps_data);
}

static void caps_robotCleanerOperatingState_init_cb(IOT_CAP_HANDLE *handle, void *usr_data)
{
    caps_robotCleanerOperatingState_data_t *caps_data = usr_data;
    if (caps_data && caps_data->init_usr_cb)
        caps_data->init_usr_cb(caps_data);
    caps_robotCleanerOperatingState_attr_operatingState_send(caps_data);
}

caps_robotCleanerOperatingState_data_t *caps_robotCleanerOperatingState_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data)
{
    caps_robotCleanerOperatingState_data_t *caps_data = NULL;
    int err;

    caps_data = malloc(sizeof(caps_robotCleanerOperatingState_data_t));
    if (!caps_data) {
        ESP_LOGE(TAG, "fail to malloc for caps_robotCleanerOperatingState_data");
        return NULL;
    }

    memset(caps_data, 0, sizeof(caps_robotCleanerOperatingState_data_t));

    caps_data->init_usr_cb = init_usr_cb;
    caps_data->usr_data = usr_data;

    caps_data->get_operatingState_value = caps_robotCleanerOperatingState_get_operatingState_value;
    caps_data->set_operatingState_value = caps_robotCleanerOperatingState_set_operatingState_value;
    caps_data->attr_operatingState_str2idx = caps_robotCleanerOperatingState_attr_operatingState_str2idx;
    caps_data->attr_operatingState_send = caps_robotCleanerOperatingState_attr_operatingState_send;
    if (ctx) {
        caps_data->handle = st_cap_handle_init(ctx, component, caps_helper_robotCleanerOperatingState.id, caps_robotCleanerOperatingState_init_cb, caps_data);
    }
    if (caps_data->handle) {
        err = st_cap_cmd_set_cb(caps_data->handle, caps_helper_robotCleanerOperatingState.cmd_start.name, caps_robotCleanerOperatingState_cmd_start_cb, caps_data);
        if (err) {
            ESP_LOGE(TAG, "fail to set cmd_cb for start of robotCleanerOperatingState");
        }
        err = st_cap_cmd_set_cb(caps_data->handle, caps_helper_robotCleanerOperatingState.cmd_pause.name, caps_robotCleanerOperatingState_cmd_pause_cb, caps_data);
        if (err) {
            ESP_LOGE(TAG, "fail to set cmd_cb for pause of robotCleanerOperatingState");
        }
        err = st_cap_cmd_set_cb(caps_data->handle, caps_helper_robotCleanerOperatingState.cmd_goHome.name, caps_robotCleanerOperatingState_cmd_goHome_cb, caps_data);
        if (err) {
            ESP_LOGE(TAG, "fail to set cmd_cb for goHome of robotCleanerOperatingState");
        }
    } else {
        ESP_LOGE(TAG, "fail to init robotCleanerOperatingState handle");
    }

    return caps_data;
}
