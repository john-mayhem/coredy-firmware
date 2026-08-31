#ifndef _CAPS_NS_WATER_LEVEL_
#define _CAPS_NS_WATER_LEVEL_

#include "st_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_ID_NS_WATER_LEVEL "musicimage12631.waterLevel"
#define CAP_ATTR_NS_WATER_LEVEL "waterLevel"
#define CAP_CMD_NS_SET_WATER_LEVEL "setWaterLevel"

enum {
    CAP_ENUM_NS_WATERLEVEL_VALUE_LOW,
    CAP_ENUM_NS_WATERLEVEL_VALUE_MEDIUM,
    CAP_ENUM_NS_WATERLEVEL_VALUE_HIGH,
    CAP_ENUM_NS_WATERLEVEL_VALUE_MAX
};

// __attribute__((unused)): this table lives in the header so both the
// capability .c and any caller can index it, but most translation units that
// include this header never touch it.
__attribute__((unused)) static const char *caps_ns_waterLevel_values[CAP_ENUM_NS_WATERLEVEL_VALUE_MAX] = {
    "low", "medium", "high"
};

typedef struct caps_ns_waterLevel_data {
    IOT_CAP_HANDLE* handle;
    void *usr_data;

    char *waterLevel_value;

    void (*set_waterLevel_value)(struct caps_ns_waterLevel_data *caps_data, const char *value);
    void (*attr_waterLevel_send)(struct caps_ns_waterLevel_data *caps_data);

    void (*init_usr_cb)(struct caps_ns_waterLevel_data *caps_data);
    void (*cmd_setWaterLevel_usr_cb)(struct caps_ns_waterLevel_data *caps_data);
} caps_ns_waterLevel_data_t;

caps_ns_waterLevel_data_t *caps_ns_waterLevel_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data);

#ifdef __cplusplus
}
#endif

#endif
