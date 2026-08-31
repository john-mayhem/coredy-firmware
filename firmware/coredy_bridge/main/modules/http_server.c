#include "http_server.h"
#include "modules/log_buffer.h"
#include "modules/unknown_store.h"
#include "modules/diag.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HTTP";

// TWO independent server instances, on purpose.
//
// esp_http_server dispatches every request from a SINGLE task (one
// httpd_thread, one select loop) and this IDF fork has no
// httpd_req_async_handler_begin(). The /logs SSE handler below never returns --
// it holds that task for as long as a browser is attached. So while anyone has
// the log viewer open, every other endpoint on that instance is unreachable.
//
// That is not theoretical: on 2026-08-31 /unknown timed out repeatedly from the
// workstation while the log page was open in a browser, and started answering
// the moment the tab was closed. Since /unknown is the machine-facing data path
// out of a sealed robot, it cannot be blockable by a human looking at logs.
//
// Giving the API its own instance gives it its own task and socket set, so the
// two are genuinely independent. Bounding the SSE handler on a timer instead
// would only make the API *probably* available; this makes it always available.
#define HTTP_LOG_PORT 80
#define HTTP_API_PORT 8080

static httpd_handle_t server = NULL;      // :80   human-facing log viewer (blocking SSE)
static httpd_handle_t api_server = NULL;  // :8080 machine-facing JSON API (never blocks)

static const char index_html[] =
"<!DOCTYPE html><html><head><title>Coredy Bridge Logs</title>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>"
"body{font-family:ui-monospace,Menlo,Consolas,monospace;background:#111;color:#ddd;margin:0;padding:8px;font-size:12px}"
".E{color:#f55}.W{color:#fa0}.I{color:#5af}.D{color:#888}"
"#st{position:fixed;top:0;right:0;padding:4px 8px;background:#222;border-bottom-left-radius:4px;font-weight:bold;z-index:10}"
"#st.live{color:#5f5}#st.off{color:#f55}"
"#log{margin-top:24px;white-space:pre-wrap;word-break:break-all}"
".row{padding:1px 2px;border-bottom:1px solid #1a1a1a}"
"a{color:#5af}"
"</style></head><body>"
"<div id=st class=off>connecting...</div>"
"<div id=api></div>"
"<div id=log></div>"
"<script>"
/* The API lives on :8080, not here -- this page's SSE stream permanently
   occupies :80's single request task, so those endpoints must not share it. */
"const A=document.getElementById('api');"
"A.innerHTML=\"API (separate port, works while this stream is open): \""
"  +\"<a href='http://\"+location.hostname+\":8080/status'>/status</a> \""
"  +\"<a href='http://\"+location.hostname+\":8080/unknown'>/unknown</a>\";"
"const L=document.getElementById('log'),S=document.getElementById('st');"
"const e=new EventSource('/logs');"
"e.onopen=()=>{S.textContent='live';S.className='live'};"
"e.onerror=()=>{S.textContent='disconnected';S.className='off'};"
"e.onmessage=ev=>{"
"  const t=ev.data,d=document.createElement('div');"
"  d.className='row '+(t.match(/ E /)?'E':t.match(/ W /)?'W':t.match(/ I /)?'I':'D');"
"  d.textContent=t;L.appendChild(d);"
"  while(L.childNodes.length>2000)L.removeChild(L.firstChild);"
"  window.scrollTo(0,document.body.scrollHeight);"
"};"
"</script></body></html>";

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_logs(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

    QueueHandle_t q = log_buffer_subscribe();
    if (!q) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no log slots");
        return ESP_FAIL;
    }

    const char *hello = "data: --- log stream connected ---\n\n";
    if (httpd_resp_send_chunk(req, hello, strlen(hello)) != ESP_OK) {
        log_buffer_unsubscribe(q);
        return ESP_FAIL;
    }

    while (1) {
        char *line = NULL;
        if (xQueueReceive(q, &line, pdMS_TO_TICKS(15000)) == pdTRUE) {
            esp_err_t e1 = httpd_resp_send_chunk(req, "data: ", 6);
            esp_err_t e2 = (e1 == ESP_OK) ? httpd_resp_send_chunk(req, line, strlen(line)) : ESP_FAIL;
            esp_err_t e3 = (e2 == ESP_OK) ? httpd_resp_send_chunk(req, "\n\n", 2) : ESP_FAIL;
            free(line);
            if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) break;
        } else {
            /* SSE comment line as keepalive. */
            if (httpd_resp_send_chunk(req, ": ka\n\n", 6) != ESP_OK) break;
        }
    }

    char *leftover;
    while (xQueueReceive(q, &leftover, 0) == pdTRUE) {
        free(leftover);
    }
    log_buffer_unsubscribe(q);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// GET /unknown         -> JSON dump of every unrecognised frame seen so far
// GET /unknown?clear=1 -> same, then empties the table
//
// This is the endpoint the workstation polls. It is the whole point of the
// logging story: an unknown DP or command has to survive until something asks
// for it, because with the robot reassembled nobody is watching the live
// stream when the interesting frame goes by.
static esp_err_t handle_unknown(httpd_req_t *req)
{
    bool clear = false;
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1 && qlen < 64) {
        char q[64];
        if (httpd_req_get_url_query_str(req, q, qlen) == ESP_OK) {
            char val[8];
            if (httpd_query_key_value(q, "clear", val, sizeof(val)) == ESP_OK && val[0] == '1') {
                clear = true;
            }
        }
    }

    const size_t cap = 6144; // 32 entries * ~140 bytes, with slack
    char *buf = malloc(cap);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    size_t n = unknown_store_render_json(buf, cap);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err;
    if (n == 0) {
        err = httpd_resp_sendstr(req, "{\"error\":\"render failed\"}");
    } else {
        err = httpd_resp_send(req, buf, n);
    }
    free(buf);

    // Clear only after the response is safely out, so a dropped connection
    // can't silently discard findings the workstation never received.
    if (clear && err == ESP_OK) unknown_store_clear();
    return err;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    char buf[768];
    size_t n = diag_render_status_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (n == 0) {
        return httpd_resp_sendstr(req, "{\"error\":\"render failed\"}");
    }
    return httpd_resp_send(req, buf, n);
}

esp_err_t http_server_start(void)
{
    if (server && api_server) return ESP_OK;

    /* ---- :80 human-facing log viewer ---- */
    if (!server) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = HTTP_LOG_PORT;
        config.ctrl_port = 32768;
        config.stack_size = 6144;
        /* SSE handlers hold the connection open. Reserve headroom for short
         * requests on top of up to 4 long-lived SSE clients. */
        config.max_open_sockets = 7;
        config.lru_purge_enable = true;

        esp_err_t err = httpd_start(&server, &config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "log httpd_start failed: %s", esp_err_to_name(err));
            server = NULL;
            return err;
        }
        httpd_uri_t index_uri = { .uri = "/",     .method = HTTP_GET, .handler = handle_index };
        httpd_uri_t logs_uri  = { .uri = "/logs", .method = HTTP_GET, .handler = handle_logs };
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &logs_uri);
        ESP_LOGI(TAG, "log viewer up on :%d", HTTP_LOG_PORT);
    }

    /* ---- :8080 machine-facing API, deliberately separate (see note above) ---- */
    if (!api_server) {
        httpd_config_t api_cfg = HTTPD_DEFAULT_CONFIG();
        api_cfg.server_port = HTTP_API_PORT;
        /* Must differ from the other instance's ctrl_port or httpd_start fails
         * with the UDP control socket already bound. */
        api_cfg.ctrl_port = 32769;
        api_cfg.stack_size = 4096;
        api_cfg.max_open_sockets = 4;
        api_cfg.lru_purge_enable = true;

        esp_err_t err = httpd_start(&api_server, &api_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "api httpd_start failed: %s", esp_err_to_name(err));
            api_server = NULL;
            return err;
        }
        httpd_uri_t unknown_uri = { .uri = "/unknown", .method = HTTP_GET, .handler = handle_unknown };
        httpd_uri_t status_uri  = { .uri = "/status",  .method = HTTP_GET, .handler = handle_status };
        httpd_register_uri_handler(api_server, &unknown_uri);
        httpd_register_uri_handler(api_server, &status_uri);
        ESP_LOGI(TAG, "API up on :%d (/unknown, /status)", HTTP_API_PORT);
    }

    return ESP_OK;
}
