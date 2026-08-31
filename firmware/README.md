# coredy_bridge — firmware source

ESP32 firmware that replaces the Coredy R750's stock Tuya WR3 Wi-Fi module. It speaks the
TuyaMCU serial protocol to the robot's STM32 (`D330-A V1.0`, STM32F072VxT6) on one side and a
SmartThings Direct Connected Device on the other.

Target: **classic ESP32** (ESP32-D0WD-V3). Not the S3 — that was the original plan, but the
hardware on hand is classic ESP32, and STDK does not support the C6 either
([why](../docs/SMARTTHINGS.md#stdk-notes)). GPIO choices avoid its strapping pins 0/2/5/12/15.

Build, flash and release procedures: [`docs/OPERATIONS.md`](../docs/OPERATIONS.md).
Wire protocol and DP map: [`docs/PROTOCOL.md`](../docs/PROTOCOL.md).

## Hardware interface

| Robot `J1` | Meaning | ESP32 |
|---|---|---|
| GND | ground | GND |
| RX | STM32 → module | GPIO16 (our TX) |
| TX | module → STM32 | GPIO17 (our RX) |
| VCC | 3.3 V | 3V3 |

**The harness on the physical unit is soldered TX/RX reversed and the firmware compensates**
(`COREDY_UART_RX_GPIO 17` / `COREDY_UART_TX_GPIO 16` in `main/modules/coredy_bridge.h`). If you
rewire it "correctly", swap those two defines back — otherwise you get zero traffic in either
direction, which is exactly how the reversal was found.

Power the board from the **3V3 pin, not VIN** — feeding 3.3 V into VIN loses ~1 V across the
onboard regulator and browns out under Wi-Fi load. Fit a 470 µF–1000 µF bulk cap plus a 0.1 µF
ceramic at 3V3/GND: that rail was sized for an RTL8710BN, and classic ESP32 pulls 300–500 mA
spikes when transmitting. Brownouts show up through the RTC reset-reason diagnostics, in the boot
log and at `:8080/status`.

## Layout

```
main/
  main.c                     app entry, capability wiring, RTC crash/boot diagnostics
  modules/
    tuya_uart.c/h            TuyaMCU frame TX/RX + parser (mutex-serialised transmit)
    coredy_bridge.c/h        DP <-> SmartThings mapping, handshake, 10 s heartbeat
    coredy_ota.c/h           signed OTA client (RSA-2048/SHA-256), rollback safety
    unknown_store.c/h        deduplicated record of unrecognised frames, served at /unknown
    caps_lock.c/h            one recursive mutex guarding all capability value storage
    log_buffer.c/h           ESP_LOG -> in-memory fan-out
    http_server.c/h          log viewer (SSE) on :80, machine API on :8080
    diag.c/h                 boot/reset diagnostics backing /status
    wifi_events.c/h          Wi-Fi/IP event logging incl. RSSI
    caps_*.c/h               one wrapper per SmartThings capability
```

Structure deliberately mirrors the SmartSwitch project's `switch_example` codebase, which is the
designated house convention for these ESP32 apps.

### Why `caps_lock` exists

The STDK capability pattern stores string attributes as a heap pointer updated with `free()` +
`strdup()`. That is safe in STDK's samples, which write each capability from one place. This app
writes from **two** tasks — `tuya_uart_rx` (a DP report from the STM32) and STDK's command task
(a tap in the app) — so without a lock one can free the string the other is publishing. Every
setter and sender takes `caps_lock`; senders snapshot the string and release before the MQTT
publish. Lock ordering is `caps_lock` → `tuya_uart` TX lock, never the reverse.

### Why `unknown_store` exists

Any DP id or command byte the bridge doesn't recognise is recorded — deduplicated on the full
value tuple, with hit counts and first/last-seen — and served as JSON from `GET :8080/unknown`
(`?clear=1` empties it after returning). Before this, unknown frames only reached the live SSE
log, so anything seen while no browser was attached was lost; with the ESP32 sealed inside the
robot, that is the normal case. Polling `/unknown` is how the remaining protocol gaps
([DP101's last two states](../docs/PROTOCOL.md#notes-on-the-tricky-ones)) get closed.

## Device identity

`main/device_info.json` and `main/onboarding_config.json` are **not** in this repo —
`device_info.json` holds the device's ED25519 private key. Copy the `.example` files and fill in
your own values; see [`docs/SMARTTHINGS.md`](../docs/SMARTTHINGS.md) for keygen and Workspace
registration.
