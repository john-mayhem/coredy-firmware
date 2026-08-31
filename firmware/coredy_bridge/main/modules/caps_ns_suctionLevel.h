#ifndef _CAPS_NS_SUCTION_LEVEL_
#define _CAPS_NS_SUCTION_LEVEL_

#include "st_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_ID_NS_SUCTION_LEVEL "musicimage12631.suctionLevel"
#define CAP_ATTR_NS_SUCTION_LEVEL "suctionLevel"
#define CAP_CMD_NS_SET_SUCTION_LEVEL "setSuctionLevel"

enum {
    CAP_ENUM_NS_SUCTIONLEVEL_VALUE_QUIET,
    CAP_ENUM_NS_SUCTIONLEVEL_VALUE_NORMAL,
    CAP_ENUM_NS_SUCTIONLEVEL_VALUE_MAX_,
    CAP_ENUM_NS_SUCTIONLEVEL_VALUE_MAX
};

// __attribute__((unused)): this table lives in the header so both the
// capability .c and any caller can index it, but most translation units that
// include this header never touch it.
__attribute__((unused)) static const char *caps_ns_suctionLevel_values[CAP_ENUM_NS_SUCTIONLEVEL_VALUE_MAX] = {
    "quiet", "normal", "max"
};

typedef struct caps_ns_suctionLevel_data {
    IOT_CAP_HANDLE* handle;
    void *usr_data;

    char *suctionLevel_value;

    void (*set_suctionLevel_value)(struct caps_ns_suctionLevel_data *caps_data, const char *value);
    void (*attr_suctionLevel_send)(struct caps_ns_suctionLevel_data *caps_data);

    void (*init_usr_cb)(struct caps_ns_suctionLevel_data *caps_data);
    void (*cmd_setSuctionLevel_usr_cb)(struct caps_ns_suctionLevel_data *caps_data);
} caps_ns_suctionLevel_data_t;

caps_ns_suctionLevel_data_t *caps_ns_suctionLevel_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data);

#ifdef __cplusplus
}
#endif

#endif
