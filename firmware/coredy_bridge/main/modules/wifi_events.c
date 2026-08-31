/* ***************************************************************************
 *
 * WiFi Events Module
 * WiFi event logging and connection tracking
 *
 ****************************************************************************/

#include "wifi_events.h"
#include "modules/http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"

static const char *TAG = "WIFI_EVENTS";

void wifi_event_handler(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_CONNECTED: {
                wifi_connected = true;
                // RSSI matters here specifically: the ESP32 ends up sealed
                // inside the robot chassis, where the antenna is far more
                // obstructed than it was on the bench. Logging it at every
                // association makes "did reassembly cost us signal?" an
                // answerable question rather than a guess.
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    ESP_LOGI(TAG, "Wi-Fi connected to AP '%s' ch=%d rssi=%d dBm",
                             (const char *)ap.ssid, ap.primary, ap.rssi);
                } else {
                    ESP_LOGI(TAG, "Wi-Fi connected to AP");
                }
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
                wifi_connected = false;
                wifi_disconnect_count++;
                ESP_LOGW(TAG, "Wi-Fi disconnected #%lu, reason=%d",
                         (unsigned long)wifi_disconnect_count, event->reason);
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            /* Idempotent -- only starts the first time. */
            http_server_start();
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            ESP_LOGW(TAG, "Lost IP");
        }
    }
}

esp_err_t wifi_events_register(void)
{
    ESP_LOGI(TAG, "Registering WiFi event handlers...");

    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WiFi event handlers registered successfully");
    return ESP_OK;
}
