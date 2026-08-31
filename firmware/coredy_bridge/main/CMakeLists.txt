idf_component_register(SRCS "main.c"
                            "modules/tuya_uart.c"
                            "modules/coredy_bridge.c"
                            "modules/coredy_ota.c"
                            "modules/log_buffer.c"
                            "modules/http_server.c"
                            "modules/wifi_events.c"
                            "modules/caps_lock.c"
                            "modules/unknown_store.c"
                            "modules/diag.c"
                            "modules/caps_battery.c"
                            "modules/caps_robotCleanerOperatingState.c"
                            "modules/caps_ns_cleaningMode.c"
                            "modules/caps_ns_suctionLevel.c"
                            "modules/caps_ns_waterLevel.c"
                            "modules/caps_ns_hardwareFault.c"
                            "modules/caps_ns_areaCleaned.c"
                            "modules/caps_ns_consumables.c"
                            "modules/caps_momentary.c"
                    INCLUDE_DIRS "."
                    REQUIRES "esp_http_server" "esp_http_client" "driver" "json" "esp_wifi" "esp_event" "esp_app_format" "nvs_flash" "bt" "app_update"
                    EMBED_FILES "device_info.json"
                                "onboarding_config.json"
                    EMBED_TXTFILES "coredy_ota_public_key.pem"
                    )

set(STDK_IOT_CORE_USE_DEFINED_CONFIG "y")

add_subdirectory($ENV{STDK_CORE_PATH} iotcore)
target_link_libraries(${COMPONENT_LIB} PUBLIC iotcore)
