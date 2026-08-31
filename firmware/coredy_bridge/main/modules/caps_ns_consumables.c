#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "st_dev.h"
#include "esp_log.h"
#include "caps_ns_consumables.h"
#include "caps_lock.h"

static const char *TAG = "CAPS_CONSUMABLES";

static void set_brush(caps_ns_consumables_data_t *caps_data, int value)
{
    if (!caps_data) return;
    caps_lock_take();
    caps_data->brush_value = value;
    caps_lock_give();
}

static void set_rollerBrush(caps_ns_consumables_data_t *caps_data, int value)
{
    if (!caps_data) return;
    caps_lock_take();
    caps_data->rollerBrush_value = value;
    caps_lock_give();
}

static void set_filter(caps_ns_consumables_data_t *caps_data, int value)
{
    if (!caps_data) return;
    caps_lock_take();
    caps_data->filter_value = value;
    caps_lock_give();
}

// NB: the parameters must not be called `attr` or `value` --
// ST_CAP_SEND_ATTR_NUMBER declares locals with exactly those names, which
// would shadow them and silently change what gets published.
static void send_one(caps_ns_consumables_data_t *caps_data, const char *attr_name, int pct)
{
    int sequence_no = -1;
    if (!caps_data || !caps_data->handle) return;

    ST_CAP_SEND_ATTR_NUMBER(caps_data->handle, (char *)attr_name, pct, CAP_UNIT_NS_PERCENT, NULL, sequence_no);
    if (sequence_no < 0)
        ESP_LOGE(TAG, "fail to send %s", attr_name);
}

static void send_brush(caps_ns_consumables_data_t *caps_data)
{
    if (!caps_data) return;
    caps_lock_take();
    int v = caps_data->brush_value;
    caps_lock_give();
    send_one(caps_data, CAP_ATTR_NS_BRUSH, v);
}

static void send_rollerBrush(caps_ns_consumables_data_t *caps_data)
{
    if (!caps_data) return;
    caps_lock_take();
    int v = caps_data->rollerBrush_value;
    caps_lock_give();
    send_one(caps_data, CAP_ATTR_NS_ROLLER_BRUSH, v);
}

static void send_filter(caps_ns_consumables_data_t *caps_data)
{
    if (!caps_data) return;
    caps_lock_take();
    int v = caps_data->filter_value;
    caps_lock_give();
    send_one(caps_data, CAP_ATTR_NS_FILTER, v);
}

static void send_all(caps_ns_consumables_data_t *caps_data)
{
    send_brush(caps_data);
    send_rollerBrush(caps_data);
    send_filter(caps_data);
}

static void init_cb(IOT_CAP_HANDLE *handle, void *usr_data)
{
    caps_ns_consumables_data_t *caps_data = usr_data;
    if (caps_data->init_usr_cb) caps_data->init_usr_cb(caps_data);
    send_all(caps_data);
}

caps_ns_consumables_data_t *caps_ns_consumables_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data)
{
    caps_ns_consumables_data_t *caps_data = malloc(sizeof(caps_ns_consumables_data_t));
    if (!caps_data) return NULL;
    memset(caps_data, 0, sizeof(caps_ns_consumables_data_t));

    caps_data->init_usr_cb = init_usr_cb;
    caps_data->usr_data = usr_data;
    caps_data->set_brush_value = set_brush;
    caps_data->set_rollerBrush_value = set_rollerBrush;
    caps_data->set_filter_value = set_filter;
    caps_data->attr_brush_send = send_brush;
    caps_data->attr_rollerBrush_send = send_rollerBrush;
    caps_data->attr_filter_send = send_filter;
    caps_data->attr_send_all = send_all;

    if (ctx) {
        caps_data->handle = st_cap_handle_init(ctx, component, CAP_ID_NS_CONSUMABLES, init_cb, caps_data);
    }
    if (!caps_data->handle) {
        ESP_LOGE(TAG, "fail to init consumables handle");
    }
    return caps_data;
}
