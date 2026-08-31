# Changelog

All releases are signed; see [README](README.md) for the OTA scheme.

## v1.4.0

- **DP120 identified as water level** (`0` low / `1` medium / `2` high, `4` when idle) — the last
  unmapped DP. It is state-gated like DP104, which is why every earlier reading was `4`.
- Added `GET :8080/dp120?value=N` (allow-listed, values 0–4) so further DP probing needs no
  firmware release.
- **Fixed socket exhaustion introduced in v1.3.0.** The two httpd instances asked for 7 + 4
  against `CONFIG_LWIP_MAX_SOCKETS=10`, before STDK's MQTT or the OTA client took any — so
  attaching a browser made OTA fail every 30 s with `esp-tls: Failed to create socket`. Watching
  logs disabled updates. LWIP raised to 16, servers cut to 4 + 3.

## v1.3.0

- **Split the machine API onto its own httpd instance on :8080.** `esp_http_server` dispatches
  from a single task and this IDF fork has no `httpd_req_async_handler_begin()`, so the
  never-returning `/logs` SSE handler blocked every other endpoint on :80.
- Added `GET :8080/status` — boot count, reset reason, unclean-reset flag, uptime, Wi-Fi
  RSSI/SSID/channel, heap free/largest/min-ever, cloud state, OTA probation. Previously all of
  this existed only in the boot banner, which is useless once the device is sealed.

## v1.2.0

Fixes from a full codebase review:

- **`tuya_send_frame()` was not thread-safe** — three `uart_write_bytes()` calls, no lock, three
  transmitting tasks. Interleaving spliced frames and the STM32 dropped both on checksum. Now
  mutex-serialised.
- **Use-after-free on capability strings.** Setters `free()`+`strdup()` and were called from both
  `tuya_uart_rx` and STDK's command task on the same object. New `caps_lock` recursive mutex;
  senders snapshot the string and release before the MQTT publish. Lock order is `caps_lock` →
  UART TX lock, never the reverse.
- **Consumables published stale zeros** — DP109/110/111 arrive as separate frames and
  `attr_send_all()` broadcast the not-yet-known siblings as `0`. Now sent per-attribute.
- **OTA compared versions with `!=`**, so republishing an older tag downgraded the fleet. Now
  semver, failing *open* on unparseable versions so a comparison bug can never block an update.
- **Rollback was cancelled on first cloud connect**, which proves nothing about whether the image
  can still fetch updates. Now cancelled only after a successful `versioninfo.json` fetch; a
  probationary image that cannot reach the update server within 5 min reboots and reverts.
- **DP112 was stored but never read** — now the operating-state fallback. **DP101 = 0** mapped.
- Deduplicated DP reports (identical values were re-published every time).
- Capability layer used `printf`, which `log_buffer` never sees (it hooks `esp_log`'s vprintf
  only) — every "fail to send X" was invisible remotely. All converted to `ESP_LOG`.
- Added `GET /unknown` — deduplicated JSON of unrecognised DPs/commands with hit counts and
  first/last-seen, held in RAM until polled. Previously unknown frames only hit the live SSE
  stream, so anything seen with no browser attached was lost — the normal case once sealed.
- Wi-Fi RSSI logged on association.

## v1.1.0

- **Fixed `heartbeat_task` stack overflow.** 2048 bytes was fine pre-refactor with plain
  `printf`, but `ESP_LOGI`'s vprintf hook adds a 240-byte formatting buffer on top of
  `hex_render()`'s buffer in the same call chain, and the first heartbeat's self-log crashed the
  instant Wi-Fi came up. Now 4096, matching `tuya_uart_rx`.
- Restructured to one module per concern under `main/modules/`, `ESP_LOG` throughout with
  per-file `TAG`, RTC_NOINIT crash/boot diagnostics, and a fatal-vs-soft error handling split.

## v1.0.0

First release. TuyaMCU bridge, nine SmartThings capabilities, signed OTA client, SSE log viewer.

OTA bugs fixed during bring-up: GitHub's release-asset 302s are not followed by
`esp_http_client_open()`+`fetch_headers()`+`read()` (only `perform()` follows redirects); the
default 512-byte `buffer_size_tx` is too small for GitHub's 924-char signed redirect URLs; and
firing OTA's TLS handshake while STDK's MQTT session is unsettled fails both, so OTA now skips a
tick unless the cloud is connected.
