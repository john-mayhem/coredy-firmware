#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "st_dev.h"
#include "esp_log.h"
#include "caps_ns_cleaningMode.h"
#include "caps_lock.h"

static const char *TAG = "CAPS_CLEANINGMODE";

static int str2idx(const char *value)
{
    int index;
    for (index = 0; index < CAP_ENUM_NS_CLEANINGMODE_VALUE_MAX; index++) {
        if (!strcmp(value, caps_ns_cleaningMode_values[index])) {
            return index;
        }
    }
    return -1;
}

static void caps_ns_cleaningMode_set_value(caps_ns_cleaningMode_data_t *caps_data, const char *value)
{
    if (!caps_data) return;
    caps_lock_take();
    if (caps_data->cleaningMode_value) free(caps_data->cleaningMode_value);
    caps_data->cleaningMode_value = strdup(value);
    caps_lock_give();
}

static void caps_ns_cleaningMode_send(caps_ns_cleaningMode_data_t *caps_data)
{
    int sequence_no = -1;
    if (!caps_data || !caps_data->handle) return;

    // Publish a snapshot rather than the live pointer: the other task may
    // free()+strdup() this string at any moment, and we must not hold the
    // capability lock across an MQTT publish.
    caps_lock_take();
    char *snapshot = caps_data->cleaningMode_value ? strdup(caps_data->cleaningMode_value) : NULL;
    caps_lock_give();
    if (!snapshot) return;

    ST_CAP_SEND_ATTR_STRING(caps_data->handle, CAP_ATTR_NS_CLEANING_MODE, snapshot, NULL, NULL, sequence_no);
    free(snapshot);
    if (sequence_no < 0)
        ESP_LOGE(TAG, "fail to send cleaningMode value");
}

static void cmd_setCleaningMode_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_ns_cleaningMode_data_t *caps_data = (caps_ns_cleaningMode_data_t *)usr_data;
    int index = str2idx(cmd_data->cmd_data[0].string);
    if (index < 0) {
        ESP_LOGW(TAG, "%s is not supported value for setCleaningMode", cmd_data->cmd_data[0].string);
        return;
    }
    caps_ns_cleaningMode_set_value(caps_data, caps_ns_cleaningMode_values[index]);
    if (caps_data->cmd_setCleaningMode_usr_cb)
        caps_data->cmd_setCleaningMode_usr_cb(caps_data);
    caps_ns_cleaningMode_send(caps_data);
}

static void init_cb(IOT_CAP_HANDLE *handle, void *usr_data)
{
    caps_ns_cleaningMode_data_t *caps_data = usr_data;
    if (caps_data->init_usr_cb) caps_data->init_usr_cb(caps_data);
    caps_ns_cleaningMode_send(caps_data);
}

caps_ns_cleaningMode_data_t *caps_ns_cleaningMode_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data)
{
    caps_ns_cleaningMode_data_t *caps_data = malloc(sizeof(caps_ns_cleaningMode_data_t));
    if (!caps_data) return NULL;
    memset(caps_data, 0, sizeof(caps_ns_cleaningMode_data_t));

    caps_data->init_usr_cb = init_usr_cb;
    caps_data->usr_data = usr_data;
    caps_data->set_cleaningMode_value = caps_ns_cleaningMode_set_value;
    caps_data->attr_cleaningMode_send = caps_ns_cleaningMode_send;

    if (ctx) {
        caps_data->handle = st_cap_handle_init(ctx, component, CAP_ID_NS_CLEANING_MODE, init_cb, caps_data);
    }
    if (caps_data->handle) {
        st_cap_cmd_set_cb(caps_data->handle, CAP_CMD_NS_SET_CLEANING_MODE, cmd_setCleaningMode_cb, caps_data);
    } else {
        ESP_LOGE(TAG, "fail to init cleaningMode handle");
    }
    return caps_data;
}
