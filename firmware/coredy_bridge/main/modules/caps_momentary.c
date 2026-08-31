#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "st_dev.h"
#include "esp_log.h"
#include "caps_momentary.h"

static const char *TAG = "CAPS_MOMENTARY";

static void cmd_push_cb(IOT_CAP_HANDLE *handle, iot_cap_cmd_data_t *cmd_data, void *usr_data)
{
    caps_momentary_data_t *caps_data = (caps_momentary_data_t *)usr_data;
    ESP_LOGI(TAG, "Find Me pushed");
    if (caps_data->cmd_push_usr_cb)
        caps_data->cmd_push_usr_cb(caps_data);
}

static void init_cb(IOT_CAP_HANDLE *handle, void *usr_data)
{
    caps_momentary_data_t *caps_data = usr_data;
    if (caps_data->init_usr_cb) caps_data->init_usr_cb(caps_data);
}

caps_momentary_data_t *caps_momentary_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data)
{
    caps_momentary_data_t *caps_data = malloc(sizeof(caps_momentary_data_t));
    if (!caps_data) return NULL;
    memset(caps_data, 0, sizeof(caps_momentary_data_t));

    caps_data->init_usr_cb = init_usr_cb;
    caps_data->usr_data = usr_data;

    if (ctx) {
        caps_data->handle = st_cap_handle_init(ctx, component, CAP_ID_MOMENTARY, init_cb, caps_data);
    }
    if (caps_data->handle) {
        st_cap_cmd_set_cb(caps_data->handle, CAP_CMD_PUSH, cmd_push_cb, caps_data);
    } else {
        ESP_LOGE(TAG, "fail to init momentary handle");
    }
    return caps_data;
}
