/* Log broadcaster -- captures every ESP_LOG line and fans it out to
 * subscribers (e.g. HTTP/SSE clients). UART logging is preserved.
 * Ported from the Maya SmartSwitch project (proven in production there). */

#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void log_buffer_init(void);

/* Subscribe to the live log stream.
 * Returns a queue that delivers char* (heap-allocated, caller must free).
 * NULL if no subscriber slots are free. */
QueueHandle_t log_buffer_subscribe(void);

void log_buffer_unsubscribe(QueueHandle_t q);

#endif
