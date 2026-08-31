/* HTTP log server. Once WiFi is up, serves:
 *   GET /     -- small HTML page that opens an SSE stream
 *   GET /logs -- text/event-stream of live log lines
 * Open http://<device-ip>/ in a browser -- the intended way to watch this
 * device once it's sealed inside the reassembled robot with no USB access. */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"

esp_err_t http_server_start(void);

#endif
