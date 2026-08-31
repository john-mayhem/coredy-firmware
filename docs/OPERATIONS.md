# Operations — build, flash, release, diagnose

## Build environment

The build tree lives on a Linux VM (`stdk-builder`) at
`~/st-device-sdk-c-ref/apps/esp32/coredy_bridge/`; this repo is its source of truth.

```bash
export STDK_CORE_PATH=/home/mayhem/st-device-sdk-c-ref/iot-core
source ~/st-device-sdk-c-ref/bsp/esp32/export.sh
cd ~/st-device-sdk-c-ref/apps/esp32/coredy_bridge
idf.py build
```

Three things about this are non-obvious and have each cost time twice:

- **The correct ESP-IDF is bundled inside STDK** at `~/st-device-sdk-c-ref/bsp/esp32` — a
  patched v5.0.7 fork registered in `~/.espressif/idf-env.json`. Its `export.sh` is at
  `bsp/esp32/export.sh`, **not** at the repo root.
- **Ignore `~/esp/esp-idf` entirely.** That is an unrelated stock v5.3.2 clone; its toolchain
  family differs (installed tools use the old `xtensa-esp32-elf` naming, not v5.3's
  `xtensa-esp-elf`), so sourcing it fails on missing `xtensa-esp-elf-gdb`.
- **`STDK_CORE_PATH` must be exported before `idf.py`** — the top-level `CMakeLists.txt` reads
  `$ENV{STDK_CORE_PATH}` directly, so without it CMake fails on
  `add_subdirectory(iotcore)` → `"iotcore" which is not an existing directory`.

**Version is one line**: `set(PROJECT_VER "x.y.z")` in the top-level `CMakeLists.txt`, read at
runtime via `esp_app_get_description()`. Bump, rebuild, re-run the release script — nothing else
changes. If `PROJECT_VER` is *unset*, the banner falls back to git-describe of the STDK repo and
reports `v2.3.2-dirty`; see the "which version is actually running" lesson below.

**Secrets**: `main/device_info.json` (ED25519 private key) and `main/onboarding_config.json`
(account ids) are gitignored; `.example` placeholders are committed. The VM holds the real ones —
a fresh clone needs them dropped in before it builds a working device.

**Headroom, as of v1.2.0**: app slot 14% free, bootloader only **7% free (2112 bytes)**. Enabling
secure boot or flash encryption later will overflow the bootloader.

## Flashing

OTA is the normal path. A wired flash is only for recovery.

```
esptool --port COM4 --baud 460800 --before no_reset --after hard_reset --chip esp32 \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x11000 build/ota_data_initial.bin \
  0x20000 build/coredy_bridge.bin
```

- `--before no_reset` uses an **underscore**, not a hyphen.
- **Flashing the app alone does not change what boots** if `otadata` points at the other slot.
  That is what the four-file flash above is for. Not erasing flash preserves NVS, so Wi-Fi
  credentials and SmartThings pairing survive it.
- The dev board has no auto-reset circuit: **hold BOOT → tap EN → hold ~2 s → release**. esptool
  toggles EN but cannot pull GPIO0, so its auto-reset fails with
  `Wrong boot mode detected (0x13)`. Download mode persists once entered, so enter it first and
  *then* run esptool — there is no timing race.
- Opening a serial monitor resets the board via the CH340's DTR/RTS wiring. Expected.
- An `esptool erase-flash` is required (not just an overwrite) when the SmartThings `vid`
  changed — see [SMARTTHINGS.md](SMARTTHINGS.md).

## Cutting a release

```bash
~/make_release.sh <version> <path-to-coredy_bridge.bin> [outdir]
# lives at ~/make_release.sh on the VM, NOT in the project dir (copy committed at ota/make_release.sh)
# private key: ~/coredy-ota-keys/coredy_ota_private.pem (override with $COREDY_OTA_PRIVATE_KEY)
# produces:    <outdir>/coredy_bridge.signed.bin + <outdir>/versioninfo.json
```

This signs and packages only — it does not publish. Publishing needs the GitHub REST API
(no `gh` CLI on either machine): copy the two files to the Windows workstation, get a token via
`git credential fill` there (returns GitHub Desktop's `gho_...` OAuth token with `repo`/`gist`/
`workflow` scopes — **this only works on Windows**, the VM has no stored GitHub credentials),
`POST /repos/.../releases`, then upload both files to the returned `upload_url`.

Devices fetch `releases/latest/download/versioninfo.json` and `.../coredy_bridge.signed.bin`, so
whichever release GitHub currently marks **latest** (most recently created) is what ships next.

**The repo must stay public.** It was created private by GitHub Desktop's default, which caused
silent 404s on every device fetch for hours — initially misdiagnosed as heap exhaustion.

## On-device HTTP

Two separate `httpd` instances, deliberately:

| Port | Endpoint | Purpose |
|---|---|---|
| **80** | `/` , `/logs` | Human log viewer, SSE, mirrors all `ESP_LOG` output |
| **8080** | `/unknown` | Deduplicated JSON of unrecognised DPs/commands with hit counts and first/last-seen. `?clear=1` empties it after returning. |
| **8080** | `/status` | Boot count, last reset reason, unclean-reset flag, uptime, Wi-Fi RSSI/SSID/channel, heap free/largest/min-ever, cloud state, OTA probation state |
| **8080** | `/dp120?value=N` | Writes DP120 directly (allow-listed to DP120, values 0–4) so further probing needs no firmware release |

They are split because `esp_http_server` dispatches every request from a **single task**, and
this IDF fork has no `httpd_req_async_handler_begin()`. The `/logs` SSE handler never returns, so
on one instance it holds that task and blocks every other endpoint — observed live: `/unknown`
timed out whenever a log page was open and answered the instant the tab closed.

**Watch the socket budget.** The two instances originally asked for 7 + 4 against
`CONFIG_LWIP_MAX_SOCKETS=10`, before STDK's MQTT or the OTA client took any — so attaching a
browser made OTA fail every 30 s with `esp-tls: Failed to create socket … sock < 0`. *Watching
logs disabled updates*, the one capability a sealed device cannot lose. Now LWIP is 16 and the
servers ask for 4 + 3. The budget is system-wide, and the OTA client must never be the one that
loses the race for a socket.

## Field diagnostics

Measured on the sealed unit, powered from the mainboard 3V3, sitting on the dock:

- RSSI **−57 to −59 dBm** — reassembly cost no signal.
- `reset_reason: SOFTWARE`, `unclean_reset: false` — the observed reboot was the OTA's own
  restart, not a brownout; the 3V3 rail holds.
- Heap ~67 KB steady, but **`heap_min_free_ever` dipped to 11276 during an OTA download** (TLS
  buffers plus the OTA write path). It completed, but that is a thin margin — and it is only
  visible because `/status` reports it. Worth watching.

**Which version is actually running is only knowable from the device.** A device once believed
stuck on v1.0.6 was in fact running an unreleased build: the boot banner read `App version:
v2.3.2-dirty`, `Compile time: Aug 31 2026 09:29:39` — a git-describe fallback from an image built
before `PROJECT_VER` was ever set, and older than every published release. "We published X most
recently, therefore it is running X" is not evidence; the banner is.

Two OTA bugs found the same way and worth remembering:
- `Out of buffer` on every fetch comes from `http_client_prepare_first_line()` and is gated
  **only** on `buffer_size_tx` — nothing to do with heap. GitHub's chain is 302 → 302 → 200 and
  the second `Location:` measured **924 chars**; IDF defaults `buffer_size_tx` to 512, so it fails
  deterministically. Now 4096 (~4.7× headroom), verified live.
- `esp_http_client_open()` + `fetch_headers()` + `read()` does **not** follow redirects; only
  `esp_http_client_perform()` does. GitHub asset URLs are 302s. Handled by an explicit
  `open_following_redirects()` loop using `esp_http_client_set_redirection()`.
