#include "unknown_store.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "UNKNOWN";

typedef struct {
    bool     used;
    uint8_t  kind;
    uint8_t  cmd;      // UNKNOWN_KIND_CMD only
    uint8_t  dpid;     // UNKNOWN_KIND_DP only
    uint8_t  dptype;   // UNKNOWN_KIND_DP only
    uint16_t len;      // bytes actually captured into value[]
    uint16_t raw_len;  // bytes seen on the wire (may exceed len if truncated)
    uint8_t  value[UNKNOWN_STORE_MAX_VALUE];
    uint32_t count;
    int64_t  first_ms;
    int64_t  last_ms;
} entry_t;

static entry_t s_entries[UNKNOWN_STORE_MAX_ENTRIES];
static uint32_t s_dropped;   // distinct signatures we had no slot for
static SemaphoreHandle_t s_lock;

void unknown_store_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    memset(s_entries, 0, sizeof(s_entries));
    s_dropped = 0;
}

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// Shared by both record_* paths. Matches on the full signature so a DP whose
// value changes shows up as separate rows -- that distinction is the whole
// point when characterising an unknown enum (see DP120, where the direction
// and value pattern is what identifies it as a rejected command rather than
// static metadata).
static void record(uint8_t kind, uint8_t cmd, uint8_t dpid, uint8_t dptype,
                   const uint8_t *data, uint16_t data_len)
{
    uint16_t keep = data_len > UNKNOWN_STORE_MAX_VALUE ? UNKNOWN_STORE_MAX_VALUE : data_len;
    int64_t now = esp_timer_get_time() / 1000;

    lock();

    int free_slot = -1;
    for (int i = 0; i < UNKNOWN_STORE_MAX_ENTRIES; i++) {
        entry_t *e = &s_entries[i];
        if (!e->used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (e->kind == kind && e->cmd == cmd && e->dpid == dpid && e->dptype == dptype &&
            e->len == keep && e->raw_len == data_len &&
            (keep == 0 || memcmp(e->value, data, keep) == 0)) {
            e->count++;
            e->last_ms = now;
            unlock();
            return;
        }
    }

    if (free_slot < 0) {
        s_dropped++;
        unlock();
        // Still shout about it -- a full table means we're seeing more novelty
        // than expected and the workstation should be polling/clearing.
        ESP_LOGW(TAG, "table full (%d entries), dropped a new signature (total dropped %lu)",
                 UNKNOWN_STORE_MAX_ENTRIES, (unsigned long)s_dropped);
        return;
    }

    entry_t *e = &s_entries[free_slot];
    e->used = true;
    e->kind = kind;
    e->cmd = cmd;
    e->dpid = dpid;
    e->dptype = dptype;
    e->len = keep;
    e->raw_len = data_len;
    if (keep) memcpy(e->value, data, keep);
    e->count = 1;
    e->first_ms = now;
    e->last_ms = now;

    unlock();
}

void unknown_store_record_dp(uint8_t dpid, uint8_t dptype, const uint8_t *value, uint16_t len)
{
    record(UNKNOWN_KIND_DP, 0, dpid, dptype, value, len);
}

void unknown_store_record_cmd(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    record(UNKNOWN_KIND_CMD, cmd, 0, 0, payload, len);
}

void unknown_store_clear(void)
{
    lock();
    memset(s_entries, 0, sizeof(s_entries));
    s_dropped = 0;
    unlock();
    ESP_LOGI(TAG, "table cleared");
}

// Appends to `out` while tracking remaining space. Returns false once anything
// would overflow, so the caller can bail out with a valid (if short) document
// rather than emitting truncated JSON.
static bool append(char *out, size_t out_size, size_t *w, const char *fmt, ...)
{
    if (*w >= out_size) return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *w, out_size - *w, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= out_size - *w) return false;
    *w += (size_t)n;
    return true;
}

size_t unknown_store_render_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;

    size_t w = 0;
    bool ok = true;

    lock();

    int used = 0;
    for (int i = 0; i < UNKNOWN_STORE_MAX_ENTRIES; i++) if (s_entries[i].used) used++;

    ok = append(out, out_size, &w,
                "{\"uptime_ms\":%lld,\"entries\":%d,\"capacity\":%d,\"dropped\":%lu,\"items\":[",
                (long long)(esp_timer_get_time() / 1000), used,
                UNKNOWN_STORE_MAX_ENTRIES, (unsigned long)s_dropped);

    bool first = true;
    for (int i = 0; ok && i < UNKNOWN_STORE_MAX_ENTRIES; i++) {
        entry_t *e = &s_entries[i];
        if (!e->used) continue;

        if (!first) ok = append(out, out_size, &w, ",");
        first = false;
        if (!ok) break;

        if (e->kind == UNKNOWN_KIND_DP) {
            ok = append(out, out_size, &w,
                        "{\"kind\":\"dp\",\"dpid\":%u,\"dptype\":%u,\"len\":%u,\"raw\":\"",
                        e->dpid, e->dptype, e->raw_len);
        } else {
            ok = append(out, out_size, &w,
                        "{\"kind\":\"cmd\",\"cmd\":%u,\"len\":%u,\"raw\":\"",
                        e->cmd, e->raw_len);
        }
        for (int b = 0; ok && b < e->len; b++) {
            ok = append(out, out_size, &w, "%02X", e->value[b]);
        }
        if (ok) {
            ok = append(out, out_size, &w,
                        "\",\"truncated\":%s,\"count\":%lu,\"first_ms\":%lld,\"last_ms\":%lld}",
                        e->raw_len > e->len ? "true" : "false",
                        (unsigned long)e->count,
                        (long long)e->first_ms, (long long)e->last_ms);
        }
    }

    if (ok) ok = append(out, out_size, &w, "]}");

    unlock();

    if (!ok) {
        ESP_LOGW(TAG, "render buffer too small (%u bytes)", (unsigned)out_size);
        return 0;
    }
    return w;
}
