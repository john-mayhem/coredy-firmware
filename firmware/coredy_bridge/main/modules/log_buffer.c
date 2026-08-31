#include "log_buffer.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define MAX_SUBS     4
#define QUEUE_DEPTH  64
#define LOG_LINE_MAX     240

typedef struct {
    QueueHandle_t q;
    bool active;
} sub_t;

static sub_t subs[MAX_SUBS];
static SemaphoreHandle_t subs_mutex;
static vprintf_like_t original_vprintf;

static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* Pass-through to UART using the original vprintf. Use a copy of args
     * because vsnprintf consumes it. */
    va_list ua;
    va_copy(ua, args);
    int n = original_vprintf ? original_vprintf(fmt, ua) : vprintf(fmt, ua);
    va_end(ua);

    /* If no subscribers, skip the broadcast work entirely. */
    if (!subs_mutex || xSemaphoreTake(subs_mutex, 0) != pdTRUE) {
        return n;
    }
    bool any = false;
    for (int i = 0; i < MAX_SUBS; i++) {
        if (subs[i].active) { any = true; break; }
    }
    if (!any) {
        xSemaphoreGive(subs_mutex);
        return n;
    }

    char buf[LOG_LINE_MAX];
    va_list ba;
    va_copy(ba, args);
    int len = vsnprintf(buf, sizeof(buf), fmt, ba);
    va_end(ba);
    if (len <= 0) {
        xSemaphoreGive(subs_mutex);
        return n;
    }
    if (len >= LOG_LINE_MAX) len = LOG_LINE_MAX - 1;

    /* Strip ESP_LOG ANSI color escapes ("\033[...m"). UART keeps the color
     * via the original_vprintf path; this only affects the web mirror. */
    int w = 0;
    for (int r = 0; r < len; r++) {
        if (buf[r] == '\033' && r + 1 < len && buf[r+1] == '[') {
            int s = r + 2;
            while (s < len && buf[s] != 'm') s++;
            r = s;            /* skip up to and including 'm' */
            continue;
        }
        buf[w++] = buf[r];
    }
    len = w;
    buf[len] = '\0';

    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[--len] = '\0';
    }
    if (len == 0) {
        xSemaphoreGive(subs_mutex);
        return n;
    }

    for (int i = 0; i < MAX_SUBS; i++) {
        if (!subs[i].active) continue;
        char *copy = malloc(len + 1);
        if (!copy) continue;
        memcpy(copy, buf, len);
        copy[len] = '\0';
        if (xQueueSend(subs[i].q, &copy, 0) != pdTRUE) {
            free(copy);
        }
    }
    xSemaphoreGive(subs_mutex);
    return n;
}

void log_buffer_init(void)
{
    subs_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_SUBS; i++) {
        subs[i].q = NULL;
        subs[i].active = false;
    }
    original_vprintf = esp_log_set_vprintf(log_vprintf_hook);
}

QueueHandle_t log_buffer_subscribe(void)
{
    QueueHandle_t result = NULL;
    xSemaphoreTake(subs_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (subs[i].active) continue;
        if (!subs[i].q) {
            subs[i].q = xQueueCreate(QUEUE_DEPTH, sizeof(char *));
        }
        if (subs[i].q) {
            subs[i].active = true;
            result = subs[i].q;
        }
        break;
    }
    xSemaphoreGive(subs_mutex);
    return result;
}

void log_buffer_unsubscribe(QueueHandle_t q)
{
    xSemaphoreTake(subs_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (subs[i].q != q) continue;
        subs[i].active = false;
        char *line;
        while (xQueueReceive(subs[i].q, &line, 0) == pdTRUE) {
            free(line);
        }
        break;
    }
    xSemaphoreGive(subs_mutex);
}
