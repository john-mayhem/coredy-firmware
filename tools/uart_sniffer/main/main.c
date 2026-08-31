// Coredy R750 recon: passive UART sniffer between the STM32 main MCU and
// the Tuya WR3 Wi-Fi module. Two hardware UARTs, both configured RX-only —
// this firmware never drives either data line, so it can safely sit as a
// third listener on a bus that already has two active drivers.
//
// Wiring (robot J1 -> ESP32-C6 SuperMini):
//   GND -> GND
//   RX  (STM32 -> module) -> GPIO6   (tagged "MCU->WiFi" below)
//   TX  (module -> STM32) -> GPIO7   (tagged "WiFi->MCU" below)
//   VCC -> not connected
//
// Log goes out over the C6's native USB-Serial/JTAG (COM3), no extra wiring
// needed for that. Change SNIFF_BAUD below and reflash if 9600 turns out
// to be wrong (try 115200 next).

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"

#define SNIFF_BAUD      115200
#define PIN_MCU_TO_WIFI 6   // robot J1 "RX" pin
#define PIN_WIFI_TO_MCU 7   // robot J1 "TX" pin
#define MAX_PAYLOAD     512

typedef enum {
    ST_HDR1, ST_HDR2, ST_VER, ST_CMD, ST_LENHI, ST_LENLO, ST_PAYLOAD, ST_CKSUM
} parse_state_t;

typedef struct {
    const char *label;
    parse_state_t state;
    uint8_t version, cmd, len_hi, len_lo;
    uint16_t len, idx;
    uint8_t payload[MAX_PAYLOAD];
    uint8_t checksum_calc;
} tuya_parser_t;

static void print_hex(const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02X ", buf[i]);
}

static const char *dp_type_name(uint8_t t) {
    switch (t) {
        case 0x00: return "raw";
        case 0x01: return "bool";
        case 0x02: return "value";
        case 0x03: return "string";
        case 0x04: return "enum";
        case 0x05: return "bitmap";
        default:   return "unknown";
    }
}

static uint32_t read_be_uint(const uint8_t *v, uint16_t len) {
    uint32_t val = 0;
    uint16_t n = len > 4 ? 4 : len;
    for (uint16_t i = 0; i < n; i++) val = (val << 8) | v[i];
    return val;
}

// Friendly names, from the verified community Tuya profile for this exact
// product (Coredy R750, product id eg0tdmbmozbtlzyg -- make-all/tuya-local
// devices/mellerware_citymove_vacuum.yaml). NULL = not in that schema.
static const char *dp_name(uint8_t id) {
    switch (id) {
        case 1:   return "power";
        case 14:  return "area_cleaned_m2";
        case 17:  return "error";
        case 101: return "status";
        case 102: return "command (clean mode)";
        case 104: return "fan_speed (suction)";
        case 105: return "direction_control";
        case 107: return "clean_time_min";
        case 108: return "battery_pct";
        case 109: return "brush_remaining_pct";
        case 110: return "roller_brush_remaining_pct";
        case 111: return "hepa_remaining_pct";
        case 112: return "activate (start/pause)";
        case 113: return "reset_brush";
        case 114: return "reset_roller_brush";
        case 115: return "reset_hepa";
        case 116: return "locate (find me)";
        case 117: return "program/time";
        case 118: return "model";
        case 119: return "data_sample";
        default:  return NULL;
    }
}

// DP102 "command" enum, in the order the schema declares it. Only index 0
// has been directly confirmed against real captured traffic (fired on an
// AUTO tap) -- the rest assume the same sequential order, unverified.
static const char *dp102_command_label(uint32_t v) {
    static const char *labels[] = {
        "auto [CONFIRMED]",
        "random [assumed order, unverified -- no UI button for this]",
        "wall_follow/edge [CONFIRMED]",
        "clean_spot/spot [CONFIRMED]",
        "clean_room/small_room [CONFIRMED]",
        "find_sta/home [CONFIRMED]",
    };
    if (v < sizeof(labels) / sizeof(labels[0])) return labels[v];
    return NULL;
}

// Empirically observed, NOT from the schema (its declared alphabetical
// order does not match the real wire indices -- proven wrong by the
// DP101=4 "returning" observation). Built up anchor by anchor as we see
// each state correlate with a known action.
static const char *dp101_status_label(uint32_t v) {
    switch (v) {
        case 1: return "paused/halted [CONFIRMED empirically]";
        case 2: return "cleaning/running [CONFIRMED empirically]";
        case 4: return "returning to base [CONFIRMED empirically]";
        case 5: return "charging [CONFIRMED empirically]";
        default: return NULL;
    }
}

// DP105 "direction_control" enum -- CONFIRMED via a deliberate
// forward/right/reverse/left sequence matching this exact order, with "4"
// (stop) firing on every D-pad release.
static const char *dp105_direction_label(uint32_t v) {
    static const char *labels[] = {
        "forward [CONFIRMED]", "reverse [CONFIRMED]", "left [CONFIRMED]",
        "right [CONFIRMED]", "stop [CONFIRMED]",
    };
    if (v < sizeof(labels) / sizeof(labels[0])) return labels[v];
    return NULL;
}

// DP104 "fan_speed" enum, order assumed from the schema -- not yet
// confirmed against real captured traffic.
static const char *dp104_fan_label(uint32_t v) {
    static const char *labels[] = {
        "Low [CONFIRMED -- accepted mid-run, ack matched requested value]",
        "Medium [CONFIRMED -- accepted mid-run, ack matched requested value]",
        "High [CONFIRMED -- accepted mid-run, ack matched requested value]",
    };
    if (v < sizeof(labels) / sizeof(labels[0])) return labels[v];
    return NULL;
}

static void print_fault_bitmap(uint32_t bits) {
    if (bits == 0) { printf("ok (no fault)"); return; }
    struct { uint32_t bit; const char *name; } faults[] = {
        {1, "cliff"}, {2, "imp"}, {4, "whl"}, {8, "brush"}, {16, "fan"},
        {32, "roller_brush"}, {64, "low_power"}, {128, "give_up"}, {256, "no_dust"},
    };
    bool first = true;
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        if (bits & faults[i].bit) {
            printf("%s%s", first ? "" : "+", faults[i].name);
            first = false;
        }
    }
    if (first) printf("unrecognized bits 0x%lX", (unsigned long)bits);
}

// Prints one DP record. Uses known semantics from the verified schema where
// we have them (flagging confirmed vs. assumed-order); falls back to a
// generic type-based dump for anything outside that schema.
static void print_dp(uint8_t dpid, uint8_t dptype, const uint8_t *val, uint16_t vlen) {
    const char *name = dp_name(dpid);
    printf("      DP#%-3u %-28s type=%s(%u) len=%u  ",
           dpid, name ? name : "(unknown)", dp_type_name(dptype), dptype, vlen);

    uint32_t v = read_be_uint(val, vlen);

    switch (dpid) {
        case 17: // error bitfield -- fully confirmed mapping
            printf("= ");
            print_fault_bitmap(v);
            printf("\n");
            return;
        case 102: {
            if (v == 6) {
                printf("= 6 (halted/obstacle-fault -- overloaded onto this DP, "
                       "outside the documented 0-5 command enum) [CONFIRMED via repeated cliff correlation]\n");
                return;
            }
            const char *lbl = dp102_command_label(v);
            printf("= %lu (%s)\n", (unsigned long)v, lbl ? lbl : "out-of-range / unknown value");
            return;
        }
        case 104: {
            const char *lbl = dp104_fan_label(v);
            printf("= %lu (%s)\n", (unsigned long)v, lbl ? lbl : "out-of-range / unknown value");
            return;
        }
        case 105: {
            const char *lbl = dp105_direction_label(v);
            printf("= %lu (%s)\n", (unsigned long)v, lbl ? lbl : "out-of-range / unknown value");
            return;
        }
        case 112:
            printf("= %lu (%s) [CONFIRMED]\n", (unsigned long)v, v ? "running/active" : "paused/stopped");
            return;
        case 108:
            printf("= %lu %%\n", (unsigned long)v);
            return;
        case 107:
            printf("= %lu min\n", (unsigned long)v);
            return;
        case 109: case 110: case 111:
            printf("= %lu %%\n", (unsigned long)v);
            return;
        case 14: // schema declares scale:10 for this DP
            printf("= %.1f m^2\n", v / 10.0);
            return;
        case 101: {
            const char *lbl = dp101_status_label(v);
            if (lbl) {
                printf("= %lu (%s)\n", (unsigned long)v, lbl);
            } else {
                printf("= raw %lu (not yet observed; known schema values, order unconfirmed: "
                       "standby/charged/paused/cleaning/cleaning_complete/returning/charging)\n",
                       (unsigned long)v);
            }
            return;
        }
        default:
            break;
    }

    // generic fallback for anything without a specific decoder above
    switch (dptype) {
        case 0x01: printf("= %s\n", v ? "true" : "false"); return;
        case 0x02: printf("= %ld\n", (long)v); return;
        case 0x03: printf("= \"%.*s\"\n", (int)vlen, (const char *)val); return;
        case 0x04: printf("= %lu\n", (unsigned long)v); return;
        default:
            printf("= ");
            print_hex(val, vlen);
            printf("\n");
            return;
    }
}

// Walks the payload as a sequence of [dpid][dptype][len_hi][len_lo][value...]
// records. Falls back to a raw dump if it doesn't fit that shape.
static void decode_payload_as_dps(const uint8_t *payload, uint16_t len) {
    size_t off = 0;
    bool decoded_any = false;

    while (off + 4 <= len) {
        uint8_t dpid = payload[off];
        uint8_t dptype = payload[off + 1];
        uint16_t dplen = (payload[off + 2] << 8) | payload[off + 3];
        if (off + 4 + dplen > len) break; // malformed -> bail to raw fallback

        print_dp(dpid, dptype, payload + off + 4, dplen);

        off += 4 + dplen;
        decoded_any = true;
    }

    if (!decoded_any) {
        printf("      payload (no DP structure recognized): ");
        print_hex(payload, len);
        printf("\n");
    } else if (off != len) {
        printf("      trailing unparsed bytes: ");
        print_hex(payload + off, len - off);
        printf("\n");
    }
}

static uint32_t heartbeat_count = 0;

static void emit_frame(tuya_parser_t *p, bool checksum_ok) {
    int64_t ms = esp_timer_get_time() / 1000;
    if (p->cmd == 0x00) { // heartbeat ping/ack, pure noise once baud is confirmed
        heartbeat_count++;
        if (heartbeat_count % 20 == 1) { // one line roughly every 5 minutes, just to prove it's alive
            printf("[%6lld.%03lld] ... %lu heartbeats so far, suppressing further ones ...\n",
                   ms / 1000, ms % 1000, (unsigned long)heartbeat_count);
        }
        return;
    }
    printf("[%6lld.%03lld] %-10s ver=%u cmd=0x%02X len=%u %s\n",
           ms / 1000, ms % 1000, p->label, p->version, p->cmd, p->len,
           checksum_ok ? "" : "*** CHECKSUM MISMATCH ***");
    printf("      raw: 55 AA %02X %02X %02X %02X ", p->version, p->cmd, p->len_hi, p->len_lo);
    print_hex(p->payload, p->len);
    printf("\n");
    if (p->len > 0) decode_payload_as_dps(p->payload, p->len);
}

static void process_byte(tuya_parser_t *p, uint8_t b) {
    switch (p->state) {
        case ST_HDR1:
            if (b == 0x55) { p->checksum_calc = b; p->state = ST_HDR2; }
            else printf("[stray %-10s] %02X\n", p->label, b);
            break;
        case ST_HDR2:
            if (b == 0xAA) { p->checksum_calc += b; p->state = ST_VER; }
            else if (b == 0x55) { /* stay, tolerate repeated 0x55 */ }
            else { printf("[stray %-10s] %02X\n", p->label, b); p->state = ST_HDR1; }
            break;
        case ST_VER:
            p->version = b; p->checksum_calc += b; p->state = ST_CMD;
            break;
        case ST_CMD:
            p->cmd = b; p->checksum_calc += b; p->state = ST_LENHI;
            break;
        case ST_LENHI:
            p->len_hi = b; p->checksum_calc += b; p->state = ST_LENLO;
            break;
        case ST_LENLO:
            p->len_lo = b; p->checksum_calc += b;
            p->len = (p->len_hi << 8) | p->len_lo;
            p->idx = 0;
            if (p->len == 0) p->state = ST_CKSUM;
            else if (p->len > MAX_PAYLOAD) {
                printf("[%-10s] frame claims len=%u, too long, resyncing\n", p->label, p->len);
                p->state = ST_HDR1;
            } else p->state = ST_PAYLOAD;
            break;
        case ST_PAYLOAD:
            p->payload[p->idx++] = b;
            p->checksum_calc += b;
            if (p->idx >= p->len) p->state = ST_CKSUM;
            break;
        case ST_CKSUM:
            emit_frame(p, p->checksum_calc == b);
            p->state = ST_HDR1;
            break;
    }
}

typedef struct {
    uart_port_t port;
    tuya_parser_t parser;
} sniff_ctx_t;

static void sniff_task(void *arg) {
    sniff_ctx_t *ctx = (sniff_ctx_t *)arg;
    uint8_t byte;
    while (1) {
        int n = uart_read_bytes(ctx->port, &byte, 1, pdMS_TO_TICKS(1000));
        if (n > 0) process_byte(&ctx->parser, byte);
    }
}

static void start_uart(uart_port_t port, int rx_pin) {
    uart_config_t cfg = {
        .baud_rate = SNIFF_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(port, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(port, UART_PIN_NO_CHANGE, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void) {
    printf("\n=== Coredy R750 UART sniffer === baud=%d\n", SNIFF_BAUD);
    printf("MCU->WiFi tap on GPIO%d, WiFi->MCU tap on GPIO%d\n\n", PIN_MCU_TO_WIFI, PIN_WIFI_TO_MCU);

    static sniff_ctx_t ctx_mcu_to_wifi = { .parser = { .label = "MCU->WiFi" } };
    static sniff_ctx_t ctx_wifi_to_mcu = { .parser = { .label = "WiFi->MCU" } };

    ctx_mcu_to_wifi.port = UART_NUM_0;
    ctx_wifi_to_mcu.port = UART_NUM_1;

    start_uart(ctx_mcu_to_wifi.port, PIN_MCU_TO_WIFI);
    start_uart(ctx_wifi_to_mcu.port, PIN_WIFI_TO_MCU);

    xTaskCreate(sniff_task, "sniff_m2w", 4096, &ctx_mcu_to_wifi, 5, NULL);
    xTaskCreate(sniff_task, "sniff_w2m", 4096, &ctx_wifi_to_mcu, 5, NULL);
}
