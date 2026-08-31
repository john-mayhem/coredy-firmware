/* ***************************************************************************
 *
 * WiFi Events Module
 * WiFi event logging and connection tracking
 *
 ****************************************************************************/

#ifndef WIFI_EVENTS_H
#define WIFI_EVENTS_H

#include "esp_event.h"
#include <stdint.h>
#include <stdbool.h>

// External global state (defined in main.c)
extern uint32_t wifi_disconnect_count;
extern bool wifi_connected;

/**
 * WiFi and IP event handler
 * Logs all WiFi state changes and connection events
 *
 * @param arg User data (unused)
 * @param event_base Event base (WIFI_EVENT or IP_EVENT)
 * @param event_id Event ID
 * @param event_data Event-specific data
 */
void wifi_event_handler(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data);

/**
 * Register WiFi event handlers
 * Registers handlers for WiFi and IP events
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_events_register(void);

#endif // WIFI_EVENTS_H
