#ifndef _CAPS_NS_CONSUMABLES_
#define _CAPS_NS_CONSUMABLES_

#include "st_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_ID_NS_CONSUMABLES "musicimage12631.consumables"
#define CAP_ATTR_NS_BRUSH "brushRemaining"
#define CAP_ATTR_NS_ROLLER_BRUSH "rollerBrushRemaining"
#define CAP_ATTR_NS_FILTER "filterRemaining"
#define CAP_UNIT_NS_PERCENT "%"

typedef struct caps_ns_consumables_data {
    IOT_CAP_HANDLE* handle;
    void *usr_data;

    int brush_value;
    int rollerBrush_value;
    int filter_value;

    void (*set_brush_value)(struct caps_ns_consumables_data *caps_data, int value);
    void (*set_rollerBrush_value)(struct caps_ns_consumables_data *caps_data, int value);
    void (*set_filter_value)(struct caps_ns_consumables_data *caps_data, int value);

    // Per-attribute senders. DP109/110/111 arrive as three separate frames, so
    // publishing all three whenever any one lands broadcasts the not-yet-known
    // siblings as 0 -- observed live 2026-08-31 painting rollerBrush=0 and
    // filter=0 into the app before the real values arrived a few ms later.
    // Send only what actually changed.
    void (*attr_brush_send)(struct caps_ns_consumables_data *caps_data);
    void (*attr_rollerBrush_send)(struct caps_ns_consumables_data *caps_data);
    void (*attr_filter_send)(struct caps_ns_consumables_data *caps_data);

    // Publishes all three at once. Only correct at init, where all three are
    // equally unknown; do not use it on the DP path.
    void (*attr_send_all)(struct caps_ns_consumables_data *caps_data);

    void (*init_usr_cb)(struct caps_ns_consumables_data *caps_data);
} caps_ns_consumables_data_t;

caps_ns_consumables_data_t *caps_ns_consumables_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data);

#ifdef __cplusplus
}
#endif

#endif
