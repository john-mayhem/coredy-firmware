#include "caps/iot_caps_helper_robotCleanerOperatingState.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct caps_robotCleanerOperatingState_data {
    IOT_CAP_HANDLE* handle;
    void *usr_data;
    void *cmd_data;

    char *operatingState_value;

    const char *(*get_operatingState_value)(struct caps_robotCleanerOperatingState_data *caps_data);
    void (*set_operatingState_value)(struct caps_robotCleanerOperatingState_data *caps_data, const char *value);
    int (*attr_operatingState_str2idx)(const char *value);
    void (*attr_operatingState_send)(struct caps_robotCleanerOperatingState_data *caps_data);

    void (*init_usr_cb)(struct caps_robotCleanerOperatingState_data *caps_data);

    void (*cmd_start_usr_cb)(struct caps_robotCleanerOperatingState_data *caps_data);
    void (*cmd_pause_usr_cb)(struct caps_robotCleanerOperatingState_data *caps_data);
    void (*cmd_goHome_usr_cb)(struct caps_robotCleanerOperatingState_data *caps_data);
} caps_robotCleanerOperatingState_data_t;

caps_robotCleanerOperatingState_data_t *caps_robotCleanerOperatingState_initialize(IOT_CTX *ctx, const char *component, void *init_usr_cb, void *usr_data);
#ifdef __cplusplus
}
#endif
