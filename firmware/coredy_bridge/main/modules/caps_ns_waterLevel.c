#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "st_dev.h"
#include "esp_log.h"
#include "caps_ns_waterLevel.h"
#include "caps_lock.h"

static const char *TAG = "CAPS_WATERLEVEL";

static int str2idx(const char *value)
{
    int index;
    for (index = 0; index < CAP_ENUM_NS_WATERLEVEL_VALUE_MAX; index++) {
        if (!strcmp(value, caps_ns_waterLevel_values[index])) {
            return index;
        }
    }
    return -1;
}

static void caps_ns_waterLevel_set_value(caps_ns_waterLevel_data_t *caps_data, const char *value)
{
    if (!caps_data) return;
    caps_lock_take();
    if (caps_data->waterLevel_value) free(caps_data->waterLevel_value);
    caps_data->waterLevel_value = strdup(value);
    caps_lock_give();
}

static void caps_ns_waterLevel_send(caps_ns_waterLevel_data_t *caps_data)
{
    int sequence_no = -1;
    if (!caps_data || !caps_data->handle) return;

    caps_lock_take();
    char *snapshot = caps_data->waterLevel_value ? strdup(caps_data->waterLevel_value) : NULL;
    caps_lock_give();
    if (!snapshot) return;

    ST_CAP_SEND_ATTR_STRING(caps_data->handle, CAP_ATTR_NS_WATER_LEVEL, snapshot, NULL, NULL, sequence_no);
    free(snapshot);
    if (sequence_no < 0)
        ESP_LOGE(TAG, "fail to send waterLevel value");
}

static void cmd_setWaterLevel_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_ns_waterLevel_data_t *caps_data = (caps_ns_waterLevel_data_t *)usr_data;
    int index = str2idx(cmd_data->cmd_data[0].string);
    if (index < 0) {
        ESP_LOGW(TAG, "%s is not supported value for setWaterLevel", cmd_data->cmd_data[0].string);
        return;
    }
    caps_ns_waterLevel_set_value(caps_data, caps_ns_waterLevel_values[index]);
    if (caps_data->cmd_setWaterLevel_usr_cb)
        caps_data->cmd_setWaterLevel_usr_cb(caps_data);
    caps_ns_waterLevel_send(caps_data);
}

static void init_cb(IOT_CAP_HANDLE *handle, void *usr_data)
{
    caps_ns_waterLevel_data_t *caps_data = usr_data;
    if (caps_data->init_usr_cb) caps_data->init_usr_cb(caps_data);
    caps_ns_waterLevel_send(caps_data);
}

caps_ns_waterLevel_data_t *caps_ns_waterLevel_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data)
{
    caps_ns_waterLevel_data_t *caps_data = malloc(sizeof(caps_ns_waterLevel_data_t));
    if (!caps_data) return NULL;
    memset(caps_data, 0, sizeof(caps_ns_waterLevel_data_t));

    caps_data->init_usr_cb = init_usr_cb;
    caps_data->usr_data = usr_data;
    caps_data->set_waterLevel_value = caps_ns_waterLevel_set_value;
    caps_data->attr_waterLevel_send = caps_ns_waterLevel_send;

    if (ctx) {
        caps_data->handle = st_cap_handle_init(ctx, component, CAP_ID_NS_WATER_LEVEL, init_cb, caps_data);
    }
    if (caps_data->handle) {
        st_cap_cmd_set_cb(caps_data->handle, CAP_CMD_NS_SET_WATER_LEVEL, cmd_setWaterLevel_cb, caps_data);
    } else {
        ESP_LOGE(TAG, "fail to init waterLevel handle");
    }
    return caps_data;
}
