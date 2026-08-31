#ifndef _CAPS_NS_AREA_CLEANED_
#define _CAPS_NS_AREA_CLEANED_

#include "st_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_ID_NS_AREA_CLEANED "musicimage12631.areaCleaned"
#define CAP_ATTR_NS_AREA "area"
#define CAP_UNIT_NS_AREA "m^2"

typedef struct caps_ns_areaCleaned_data {
    IOT_CAP_HANDLE* handle;
    void *usr_data;

    double area_value;

    void (*set_area_value)(struct caps_ns_areaCleaned_data *caps_data, double value);
    void (*attr_area_send)(struct caps_ns_areaCleaned_data *caps_data);

    void (*init_usr_cb)(struct caps_ns_areaCleaned_data *caps_data);
} caps_ns_areaCleaned_data_t;

caps_ns_areaCleaned_data_t *caps_ns_areaCleaned_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data);

#ifdef __cplusplus
}
#endif

#endif
