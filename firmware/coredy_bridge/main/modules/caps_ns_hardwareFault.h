#ifndef _CAPS_NS_HARDWARE_FAULT_
#define _CAPS_NS_HARDWARE_FAULT_

#include "st_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_ID_NS_HARDWARE_FAULT "musicimage12631.hardwareFault"
#define CAP_ATTR_NS_SUMMARY "summary"

typedef struct caps_ns_hardwareFault_data {
    IOT_CAP_HANDLE* handle;
    void *usr_data;

    char *summary_value;

    void (*set_summary_value)(struct caps_ns_hardwareFault_data *caps_data, const char *value);
    void (*attr_summary_send)(struct caps_ns_hardwareFault_data *caps_data);

    void (*init_usr_cb)(struct caps_ns_hardwareFault_data *caps_data);
} caps_ns_hardwareFault_data_t;

caps_ns_hardwareFault_data_t *caps_ns_hardwareFault_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data);

#ifdef __cplusplus
}
#endif

#endif
