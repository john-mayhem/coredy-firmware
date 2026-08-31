#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "tuya_uart.h"

static const char *TAG = "TUYA_UART";

// Serialises tuya_send_frame(). A frame goes out as three separate
// uart_write_bytes() calls (header, payload, checksum) and there are three
// tasks that transmit: heartbeat_task every 10s, STDK's command task via
// coredy_bridge_cmd_*(), and the main task's boot handshake. Two of those
// interleaving splices one frame into the middle of another; the STM32 then
// drops both on checksum and the command silently does nothing. Rare, but it
// is exactly the "I tapped Start and nothing happened" signature that already
// cost a debugging session once (the 0x00 echo storm).
static SemaphoreHandle_t s_tx_lock;

#define TUYA_UART_PORT   UART_NUM_1
#define TUYA_UART_BAUD   115200
#define TUYA_MAX_PAYLOAD 512
#define TUYA_OUR_VERSION 0x00 // confirmed: our own (module-role) frames stamp ver=0x00, matching the real WR3

static tuya_dp_cb_t s_dp_cb = NULL;
static tuya_cmd_cb_t s_cmd_cb = NULL;

typedef enum {
    ST_HDR1, ST_HDR2, ST_VER, ST_CMD, ST_LENHI, ST_LENLO, ST_PAYLOAD, ST_CKSUM
} parse_state_t;

typedef struct {
    parse_state_t state;
    uint8_t version, cmd, len_hi, len_lo;
    uint16_t len, idx;
    uint8_t payload[TUYA_MAX_PAYLOAD];
    uint8_t checksum_calc;
} tuya_parser_t;

static tuya_parser_t s_parser;

void tuya_uart_set_dp_callback(tuya_dp_cb_t cb) { s_dp_cb = cb; }
void tuya_uart_set_cmd_callback(tuya_cmd_cb_t cb) { s_cmd_cb = cb; }

// Renders a byte buffer as "XX XX XX ..." into a caller buffer, truncating
// (with a trailing "...") rather than overflowing if it doesn't fit.
static void hex_render(char *out, size_t out_size, const uint8_t *buf, uint16_t len)
{
    size_t w = 0;
    for (uint16_t i = 0; i < len && w + 4 < out_size; i++) {
        w += snprintf(out + w, out_size - w, "%02X ", buf[i]);
    }
    if (w >= out_size - 4 && len > 0) {
        snprintf(out + (out_size > 4 ? out_size - 4 : 0), out_size > 4 ? 4 : 0, "...");
    } else if (w > 0) {
        out[w - 1] = '\0'; // trim trailing space
    } else {
        out[0] = '\0';
    }
}

void tuya_send_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[6] = {
        0x55, 0xAA, TUYA_OUR_VERSION, cmd,
        (uint8_t)(len >> 8), (uint8_t)(len & 0xFF)
    };

    uint8_t checksum = 0;
    for (int i = 0; i < 6; i++) checksum += hdr[i];
    for (uint16_t i = 0; i < len; i++) checksum += payload[i];

    char hex[196];
    hex_render(hex, sizeof(hex), payload, len);
    ESP_LOGI(TAG, "TX cmd=0x%02X len=%u raw=55 AA %02X %02X %02X %02X %s %02X",
             cmd, len, hdr[2], hdr[3], hdr[4], hdr[5], hex, checksum);

    if (s_tx_lock) xSemaphoreTake(s_tx_lock, portMAX_DELAY);

    int n1 = uart_write_bytes(TUYA_UART_PORT, (const char *)hdr, sizeof(hdr));
    int n2 = 0;
    if (len > 0 && payload) {
        n2 = uart_write_bytes(TUYA_UART_PORT, (const char *)payload, len);
    }
    int n3 = uart_write_bytes(TUYA_UART_PORT, (const char *)&checksum, 1);

    if (s_tx_lock) xSemaphoreGive(s_tx_lock);

    if (n1 < 0 || n2 < 0 || n3 < 0) {
        ESP_LOGE(TAG, "TX FAILED (uart_write_bytes returned %d/%d/%d)", n1, n2, n3);
    }
}

uint16_t tuya_dp_append_bool(uint8_t *buf, uint16_t off, uint8_t dpid, bool value)
{
    buf[off++] = dpid;
    buf[off++] = TUYA_DP_TYPE_BOOL;
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    buf[off++] = value ? 0x01 : 0x00;
    return off;
}

uint16_t tuya_dp_append_enum(uint8_t *buf, uint16_t off, uint8_t dpid, uint8_t value)
{
    buf[off++] = dpid;
    buf[off++] = TUYA_DP_TYPE_ENUM;
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    buf[off++] = value;
    return off;
}

uint16_t tuya_dp_append_value(uint8_t *buf, uint16_t off, uint8_t dpid, int32_t value)
{
    buf[off++] = dpid;
    buf[off++] = TUYA_DP_TYPE_VALUE;
    buf[off++] = 0x00;
    buf[off++] = 0x04;
    buf[off++] = (uint8_t)((value >> 24) & 0xFF);
    buf[off++] = (uint8_t)((value >> 16) & 0xFF);
    buf[off++] = (uint8_t)((value >> 8) & 0xFF);
    buf[off++] = (uint8_t)(value & 0xFF);
    return off;
}

void tuya_send_cmd06_bool(uint8_t dpid, bool value)
{
    uint8_t buf[5];
    uint16_t n = tuya_dp_append_bool(buf, 0, dpid, value);
    tuya_send_frame(0x06, buf, n);
}

void tuya_send_cmd06_enum(uint8_t dpid, uint8_t value)
{
    uint8_t buf[5];
    uint16_t n = tuya_dp_append_enum(buf, 0, dpid, value);
    tuya_send_frame(0x06, buf, n);
}

// Walks a cmd=0x07 payload as a sequence of [dpid][dptype][len_hi][len_lo][value...]
// records, firing s_dp_cb for each. Mirrors the parsing logic already proven
// out in firmware/uart_sniffer/main.c.
static void dispatch_dp_payload(const uint8_t *payload, uint16_t len)
{
    size_t off = 0;
    while (off + 4 <= len) {
        uint8_t dpid = payload[off];
        uint8_t dptype = payload[off + 1];
        uint16_t dplen = (payload[off + 2] << 8) | payload[off + 3];
        if (off + 4 + dplen > len) break; // malformed, bail

        if (s_dp_cb) {
            tuya_dp_t dp = { .dpid = dpid, .dptype = dptype, .len = dplen, .value = payload + off + 4 };
            s_dp_cb(&dp);
        }
        off += 4 + dplen;
    }
}

static void handle_frame(tuya_parser_t *p, bool checksum_ok)
{
    if (!checksum_ok) {
        ESP_LOGW(TAG, "checksum mismatch on cmd=0x%02X len=%u, dropping frame", p->cmd, p->len);
        return;
    }
    if (p->cmd == 0x07) {
        dispatch_dp_payload(p->payload, p->len);
        return;
    }
    if (s_cmd_cb) s_cmd_cb(p->cmd, p->payload, p->len);
}

static void process_byte(tuya_parser_t *p, uint8_t b)
{
    switch (p->state) {
        case ST_HDR1:
            if (b == 0x55) { p->checksum_calc = b; p->state = ST_HDR2; }
            break;
        case ST_HDR2:
            if (b == 0xAA) { p->checksum_calc += b; p->state = ST_VER; }
            else if (b != 0x55) { p->state = ST_HDR1; }
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
            else if (p->len > TUYA_MAX_PAYLOAD) p->state = ST_HDR1; // resync, frame too long to be real
            else p->state = ST_PAYLOAD;
            break;
        case ST_PAYLOAD:
            p->payload[p->idx++] = b;
            p->checksum_calc += b;
            if (p->idx >= p->len) p->state = ST_CKSUM;
            break;
        case ST_CKSUM:
            handle_frame(p, p->checksum_calc == b);
            p->state = ST_HDR1;
            break;
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t byte;
    while (1) {
        int n = uart_read_bytes(TUYA_UART_PORT, &byte, 1, portMAX_DELAY);
        if (n > 0) process_byte(&s_parser, byte);
    }
}

void tuya_uart_init(int tx_gpio, int rx_gpio)
{
    // Created before the driver so the very first frame (the boot handshake,
    // sent from coredy_bridge_init immediately after this returns) is already
    // covered.
    if (!s_tx_lock) s_tx_lock = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate = TUYA_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(TUYA_UART_PORT, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(TUYA_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(TUYA_UART_PORT, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    memset(&s_parser, 0, sizeof(s_parser));
    xTaskCreate(rx_task, "tuya_uart_rx", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "UART1 started: tx=%d rx=%d baud=%d", tx_gpio, rx_gpio, TUYA_UART_BAUD);
}
