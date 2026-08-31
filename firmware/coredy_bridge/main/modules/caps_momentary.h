#ifndef _CAPS_MOMENTARY_
#define _CAPS_MOMENTARY_

#include "st_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_ID_MOMENTARY "momentary"
#define CAP_CMD_PUSH "push"

typedef struct caps_momentary_data {
    IOT_CAP_HANDLE* handle;
    void *usr_data;

    void (*init_usr_cb)(struct caps_momentary_data *caps_data);
    void (*cmd_push_usr_cb)(struct caps_momentary_data *caps_data);
} caps_momentary_data_t;

caps_momentary_data_t *caps_momentary_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data);

#ifdef __cplusplus
}
#endif

#endif
