#ifndef _CAPS_NS_CLEANING_MODE_
#define _CAPS_NS_CLEANING_MODE_

#include "st_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_ID_NS_CLEANING_MODE "musicimage12631.cleaningMode"
#define CAP_ATTR_NS_CLEANING_MODE "cleaningMode"
#define CAP_CMD_NS_SET_CLEANING_MODE "setCleaningMode"

enum {
    CAP_ENUM_NS_CLEANINGMODE_VALUE_AUTO,
    CAP_ENUM_NS_CLEANINGMODE_VALUE_EDGE,
    CAP_ENUM_NS_CLEANINGMODE_VALUE_SPOT,
    CAP_ENUM_NS_CLEANINGMODE_VALUE_SMALLROOM,
    CAP_ENUM_NS_CLEANINGMODE_VALUE_HOME,
    CAP_ENUM_NS_CLEANINGMODE_VALUE_MAX
};

// __attribute__((unused)): this table lives in the header so both the
// capability .c and any caller can index it, but most translation units that
// include this header never touch it.
__attribute__((unused)) static const char *caps_ns_cleaningMode_values[CAP_ENUM_NS_CLEANINGMODE_VALUE_MAX] = {
    "auto", "edge", "spot", "smallRoom", "home"
};

typedef struct caps_ns_cleaningMode_data {
    IOT_CAP_HANDLE* handle;
    void *usr_data;

    char *cleaningMode_value;

    void (*set_cleaningMode_value)(struct caps_ns_cleaningMode_data *caps_data, const char *value);
    void (*attr_cleaningMode_send)(struct caps_ns_cleaningMode_data *caps_data);

    void (*init_usr_cb)(struct caps_ns_cleaningMode_data *caps_data);
    void (*cmd_setCleaningMode_usr_cb)(struct caps_ns_cleaningMode_data *caps_data);
} caps_ns_cleaningMode_data_t;

caps_ns_cleaningMode_data_t *caps_ns_cleaningMode_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data);

#ifdef __cplusplus
}
#endif

#endif
