#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "st_dev.h"
#include "esp_log.h"
#include "caps_ns_hardwareFault.h"
#include "caps_lock.h"

static const char *TAG = "CAPS_HARDWAREFAULT";

static void caps_ns_hardwareFault_set_value(caps_ns_hardwareFault_data_t *caps_data, const char *value)
{
    if (!caps_data) return;
    caps_lock_take();
    if (caps_data->summary_value) free(caps_data->summary_value);
    caps_data->summary_value = strdup(value);
    caps_lock_give();
}

static void caps_ns_hardwareFault_send(caps_ns_hardwareFault_data_t *caps_data)
{
    int sequence_no = -1;
    if (!caps_data || !caps_data->handle) return;

    caps_lock_take();
    char *snapshot = caps_data->summary_value ? strdup(caps_data->summary_value) : NULL;
    caps_lock_give();
    if (!snapshot) return;

    ST_CAP_SEND_ATTR_STRING(caps_data->handle, CAP_ATTR_NS_SUMMARY, snapshot, NULL, NULL, sequence_no);
    free(snapshot);
    if (sequence_no < 0)
        ESP_LOGE(TAG, "fail to send hardwareFault summary");
}

static void init_cb(IOT_CAP_HANDLE *handle, void *usr_data)
{
    caps_ns_hardwareFault_data_t *caps_data = usr_data;
    if (caps_data->init_usr_cb) caps_data->init_usr_cb(caps_data);
    caps_ns_hardwareFault_send(caps_data);
}

caps_ns_hardwareFault_data_t *caps_ns_hardwareFault_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data)
{
    caps_ns_hardwareFault_data_t *caps_data = malloc(sizeof(caps_ns_hardwareFault_data_t));
    if (!caps_data) return NULL;
    memset(caps_data, 0, sizeof(caps_ns_hardwareFault_data_t));

    caps_data->init_usr_cb = init_usr_cb;
    caps_data->usr_data = usr_data;
    caps_data->set_summary_value = caps_ns_hardwareFault_set_value;
    caps_data->attr_summary_send = caps_ns_hardwareFault_send;

    if (ctx) {
        caps_data->handle = st_cap_handle_init(ctx, component, CAP_ID_NS_HARDWARE_FAULT, init_cb, caps_data);
    }
    if (!caps_data->handle) {
        ESP_LOGE(TAG, "fail to init hardwareFault handle");
    }
    return caps_data;
}
