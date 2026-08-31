#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "st_dev.h"
#include "esp_log.h"
#include "caps_ns_areaCleaned.h"

static const char *TAG = "CAPS_AREACLEANED";

static void caps_ns_areaCleaned_set_value(caps_ns_areaCleaned_data_t *caps_data, double value)
{
    if (!caps_data) return;
    caps_data->area_value = value;
}

static void caps_ns_areaCleaned_send(caps_ns_areaCleaned_data_t *caps_data)
{
    int sequence_no = -1;
    if (!caps_data || !caps_data->handle) return;

    ST_CAP_SEND_ATTR_NUMBER(caps_data->handle, CAP_ATTR_NS_AREA, caps_data->area_value, CAP_UNIT_NS_AREA, NULL, sequence_no);
    if (sequence_no < 0)
        ESP_LOGE(TAG, "fail to send area value");
}

static void init_cb(IOT_CAP_HANDLE *handle, void *usr_data)
{
    caps_ns_areaCleaned_data_t *caps_data = usr_data;
    if (caps_data->init_usr_cb) caps_data->init_usr_cb(caps_data);
    caps_ns_areaCleaned_send(caps_data);
}

caps_ns_areaCleaned_data_t *caps_ns_areaCleaned_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data)
{
    caps_ns_areaCleaned_data_t *caps_data = malloc(sizeof(caps_ns_areaCleaned_data_t));
    if (!caps_data) return NULL;
    memset(caps_data, 0, sizeof(caps_ns_areaCleaned_data_t));

    caps_data->init_usr_cb = init_usr_cb;
    caps_data->usr_data = usr_data;
    caps_data->set_area_value = caps_ns_areaCleaned_set_value;
    caps_data->attr_area_send = caps_ns_areaCleaned_send;

    if (ctx) {
        caps_data->handle = st_cap_handle_init(ctx, component, CAP_ID_NS_AREA_CLEANED, init_cb, caps_data);
    }
    if (!caps_data->handle) {
        ESP_LOGE(TAG, "fail to init areaCleaned handle");
    }
    return caps_data;
}
