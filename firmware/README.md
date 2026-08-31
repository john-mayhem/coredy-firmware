# coredy_bridge — firmware source

ESP32 firmware that **replaces** the Coredy R750's stock Tuya WR3 Wi-Fi module. It speaks
the TuyaMCU serial protocol to the robot's STM32 (`D330-A V1.0`, STM32F072VxT6) on one side
and a SmartThings Direct Connected Device on the other. No Tuya cloud is involved anywhere.

Target: **classic ESP32** (ESP32-D0WD-V3). Not the S3 — that was the original plan, but the
hardware on hand is classic ESP32, and the GPIO choices below avoid its strapping pins
0/2/5/12/15.

## Hardware interface

| Robot `J1` | Meaning | ESP32 |
|---|---|---|
| GND | ground | GND |
| RX | STM32 → module | GPIO16 (our TX) |
| TX | module → STM32 | GPIO17 (our RX) |
| VCC | 3.3V | 3V3 |

**The harness on the physical unit is soldered TX/RX reversed, and the firmware compensates**
(`COREDY_UART_RX_GPIO 17` / `COREDY_UART_TX_GPIO 16` in `main/modules/coredy_bridge.h`). If
you rewire it "correctly", swap those two defines back or you will get zero traffic in
either direction.

Power the board from the **3V3 pin**, not VIN — dropping 3.3V into VIN loses ~1V across the
onboard regulator and browns out under Wi-Fi load. A 470µF–1000µF bulk cap plus 0.1µF ceramic
at the 3V3/GND pins is strongly recommended; that rail was sized for an RTL8710BN, and classic
ESP32 pulls 300–500mA spikes when transmitting.

## Layout

```
main/
  main.c                     app entry, capability wiring, RTC crash/boot diagnostics
  modules/
    tuya_uart.c/h            TuyaMCU frame TX/RX + parser (mutex-serialised transmit)
    coredy_bridge.c/h        DP <-> SmartThings capability mapping, handshake, heartbeat
    coredy_ota.c/h           signed OTA client (RSA-2048/SHA-256), rollback safety
    unknown_store.c/h        deduplicated record of unrecognised frames, served at /unknown
    caps_lock.c/h            one recursive mutex guarding all capability value storage
    log_buffer.c/h           ESP_LOG -> in-memory fan-out
    http_server.c/h          log viewer (SSE) + /unknown JSON endpoint
    wifi_events.c/h          Wi-Fi/IP event logging incl. RSSI
    caps_*.c/h               one wrapper per SmartThings capability
```

### Why `caps_lock` exists

The STDK-generated capability pattern stores string attributes as a heap pointer updated with
`free()` + `strdup()`. That is safe in STDK's samples, which write each capability from one
place. This app writes from **two** tasks — `tuya_uart_rx` (a DP report from the STM32) and
STDK's command task (a tap in the app) — so without a lock one can free the string the other
is publishing. Every setter and sender takes `caps_lock`; senders snapshot the string and
release the lock before the MQTT publish.

Lock ordering is `caps_lock` → `tuya_uart` TX lock, never the reverse.

### Why `unknown_store` exists

Any DP id or command byte the bridge doesn't recognise is recorded (deduplicated on the full
value tuple, with hit counts and first/last-seen) and served as JSON from `GET /unknown`.
Before this, unknown frames only went to the live SSE log — which means anything seen while
no browser was attached was lost. With the ESP32 sealed inside the reassembled robot that is
the normal case. `GET /unknown?clear=1` empties the table after returning it.

This is how the remaining protocol gaps get closed: the workstation polls `/unknown`, and
anything new there drives the next firmware release.

## Building

Requires the ESP-IDF **bundled inside STDK** (a patched v5.0.7 fork), not a stock IDF:

```bash
export STDK_CORE_PATH=<path>/st-device-sdk-c-ref/iot-core
source <path>/st-device-sdk-c-ref/bsp/esp32/export.sh
cd <path>/st-device-sdk-c-ref/apps/esp32/coredy_bridge
idf.py build
```

`STDK_CORE_PATH` must be exported before `idf.py`, or CMake fails on
`add_subdirectory(iotcore)`. Version is a single source of truth: `set(PROJECT_VER ...)` in
the top-level `CMakeLists.txt`, read at runtime via `esp_app_get_description()`.

## Device identity

`main/device_info.json` and `main/onboarding_config.json` are **not** in this repo —
`device_info.json` holds the device's ED25519 private key. Copy the `.example` files and fill
in your own values (`iot-core/tools/keygen/stdk-keygen.py --mnid <id> --firmware <label>`,
then register the serial and public key as a Test Device in the SmartThings Developer
Workspace).

## Flashing

OTA is the normal path. A full four-file flash is only needed to recover a device whose
`otadata` points at the wrong slot — flashing the app alone will **not** change what boots:

```
esptool --port <PORT> --baud 460800 --before no_reset --after hard_reset --chip esp32 \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x11000 build/ota_data_initial.bin \
  0x20000 build/coredy_bridge.bin
```

Not erasing flash preserves NVS, so Wi-Fi credentials and SmartThings pairing survive. The
dev board has no auto-reset circuit: enter download mode by holding BOOT, tapping EN, holding
~2s more, then releasing.
