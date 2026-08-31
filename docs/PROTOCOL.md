# Coredy R750 — hardware and TuyaMCU protocol

Reverse-engineering reference for the robot itself. Everything here was confirmed against
real captured traffic on a physical unit unless explicitly marked otherwise.

## Hardware

**Main drive/sensor board** — `D330-A V1.0`, dated `20180517`
- MCU: **STM32F072VxT6** (LQFP64, Cortex-M0)
- SWD header `J7` (`3.3V / GND / CLK / DIO`) sits next to the MCU — untested, the avenue if
  a stock-firmware dump is ever wanted.

**Wi-Fi daughterboard** — `C200-W V1.1`, dated `20180917` — *this is the board the bridge replaces*
- Module: **Tuya WR3**, FCC ID `2ANDL-WR3` (Hangzhou Tuya's own reference module, not a
  Coredy design), silicon **Realtek RTL8710BN**, module firmware `v1.0.1`
- Connector `J1` silkscreened `GND RX TX VCC` — no probing needed to find the UART.

**Battery**: 4S Li-ion, 14.8 V nominal / 16.8 V full, 3200 mAh (~47 Wh).
**Charger**: 19 V 0.6 A, dock `DS1906`.

## Wire format

**115200 8N1.** (9600 was the first guess and is wrong — frames only appear at 115200.)

```
55 AA <ver> <cmd> <len_hi> <len_lo> <payload...> <checksum>
```
Checksum = sum of all preceding bytes mod 256.

DP records inside a payload, one or more per frame:
```
<dpid> <dptype> <len_hi> <len_lo> <value...>
```
DP types: `0x01` bool, `0x02` 4-byte big-endian int, `0x03` string, `0x04` enum (1 byte),
`0x05` bitmap.

### Command bytes

The **module** is the generic, reusable half — it does not inherently know what product it is
attached to — so it drives the handshake and the STM32 answers. The bridge plays the module role.

| Cmd | Direction | Meaning |
|---|---|---|
| `0x00` | both | Heartbeat ping/ack. Stock WR3 sends one every **15.0 s** per side (measured over a 2100 s capture); the bridge uses 10 s. Direction is genuinely symmetric, so **do not auto-echo it** — see the echo-loop note below. |
| `0x01` | module → STM32 | Product info. Module sends an empty query; STM32 answers with literal ASCII JSON: `{"p":"eg0tdmbmozbtlzyg","v":"1.0.0","m":0}` |
| `0x02` | module → STM32 | MCU conf / working mode query. Module sends empty, STM32 empty-acks. |
| **`0x03`** | **module → STM32** | **Network state, 1-byte enum.** Not boilerplate — see below. |
| **`0x05`** | **STM32 → module** | **Network reset**, payload fixed `0x00`. See below. |
| **`0x06`** | **module → STM32** | **Command down** — DP payload, what fires when a control is tapped. |
| **`0x07`** | **STM32 → module** | **Status report** — DP payload, spontaneous and in answer to `0x08`. Never acked. |
| `0x08` | module → STM32 | "Query all" — STM32 dumps every DP it holds in one burst. |

**Version byte**: the module's frames stamp `ver=0x00` and the STM32's stamp `ver=0x03`. That is
each side's own fixed byte, *not* a negotiated protocol version. The bridge stamps `0x00` to
match the real WR3 and does not validate the incoming one.

### `0x03` — network state (the Wi-Fi icon)

`0` pairing/smartconfig · `2` connecting · `3` router-connected · `4` cloud-connected.

The module is the side that actually knows the connection state, so **it must push this enum
itself as its own state progresses** — the STM32 does not derive it, and it drives the robot's
Wi-Fi icon LED directly off this value. Push it wrong and the icon lies. `0` and `4` are
confirmed anchors (fired exactly on entering pairing mode, and on cloud connect); `2` and `3` are
best-effort mappings in the bridge, not independently observed.

### `0x05` — network reset

The STM32 sends this when the **physical button is long-pressed** (confirmed via two independent
long-presses). The module empty-acks and then performs a real onboarding reset — the stock WR3
was observed replaying its whole `0x01`/`0x02` handshake and announcing `0x03` state `0`
immediately afterward. A bridge that only acks and does nothing will silently ignore the user's
factory-reset gesture.

## DP map

The `p` field above is the Tuya product id `eg0tdmbmozbtlzyg`, listed in the community
[tuya-local](https://github.com/make-all/tuya-local) integration as
`mellerware_citymove_vacuum.yaml`. That gave the DP→concept map for free. **A schema declaring
a DP exists and a schema declaring its value order are two different claims** — several of the
latter turned out wrong (see DP101), so every row below was re-derived from the wire.

| DP | Name | Type | Values |
|---|---|---|---|
| 1 | `power` | bool | `true` while awake. `false` never observed — likely physical-switch-only. |
| 14 | `area_cleaned` | value | raw ÷ 10 = m² |
| **17** | `error` | bitfield | `0` ok · `1` cliff · `2` imp · `4` whl · `8` brush · `16` fan · `32` roller_brush · `64` low_power · `128` give_up · `256` no_dust — exact bits verified live against real cliff faults |
| **101** | `status` | enum | `0` standby · `1` paused · `2` cleaning · `4` returning to base · `5` charging |
| **102** | `command` (cleaning mode) | enum | `0` auto · `2` wall_follow/edge · `3` spot · `4` small_room · `5` find_sta/home · `6` cliff-fault |
| **104** | `fan_speed` (suction) | enum | `0` Low · `1` Medium · `2` High — **state-gated**, see below |
| 107 | `clean_time` | value | minutes |
| 108 | `battery` | value | % |
| 109 / 110 / 111 | brush / roller-brush / HEPA remaining | value | % (84 / 92 / 84 at capture time) |
| **105** | `direction_control` | enum | `0` forward · `1` reverse · `2` left · `3` right · `4` stop |
| **112** | `activate` (start/pause) | enum | `0` paused · `1` running |
| 113 / 114 / 115 | reset brush / roller-brush / HEPA | bool | trigger — schema-only, untested |
| **116** | `locate` (Find Me) | bool | `true` — fired exactly on the Find Me tap in isolation |
| 117 | `program` | string | opaque schedule blob. Not pursued — better handled by SmartThings Routines than replicated. |
| 118 | `model` | value | `300`, static metadata |
| 119 | `data_sample` | value | `-1` sentinel, inert |
| **120** | `water_level` | enum | `0` low · `1` medium · `2` high; reads `4` when idle — **state-gated**, see below |

### Notes on the tricky ones

**DP101 order is empirical, not schematic.** The YAML's alphabetical ordering does *not* match
the wire indices — proven by the `returning`=4 anchor. Five of eight states are confirmed;
`charged`, `cleaning_complete` and one further charging variant still need a full charge cycle
and a clean run that actually finishes. The firmware logs any unmapped value as
`DP101=N is an unconfirmed state`.

**DP102 `6` is undocumented and Coredy-specific.** It fires whenever the cliff sensor trips
mid-clean. Not in the shared Mellerware schema. DP102=`5` *is* the docking control — there is
no separate go-home DP. `1` (random) exists in the schema with no UI button; untested.

**DP120 was the last DP solved**, and two earlier readings of it were both wrong: it is neither
static metadata nor permanently rejected. Writing while the robot was **running**: `1`→`1`
accepted, `2`→`2` accepted, `3`→`1` rejected, reverting to `4` the moment it stops. `0` is
inferred, not observed — the stock app offered exactly three water levels, and with 1/2 valid
and 3 invalid, 0/1/2 is the only fit. Selecting `low` in the app while running (or
`GET :8080/dp120?value=0`) settles it.

## Behavioural quirks

- **Suction (DP104) and water level (DP120) are state-gated.** They are only accepted while the
  robot is genuinely running — not paused, faulted or charging. **A rejected write is echoed
  back with the *previous* value**, and that mismatch is the only signal that it was rejected.
  Every early misreading of DP120 traces to nothing ever having written to it while moving.
- **The top physical button is wired straight to the STM32** and never touches the Wi-Fi module.
  Pressing it produces an MCU→Wi-Fi status report with **no preceding command frame** — which
  reads as a spontaneous, unattributed state change. Use app-only taps when correlating a new DP.
- **Bench testing re-triggers cliff faults.** Wheels off the ground, the cliff sensor can never
  be satisfied, so navigation-dependent modes fault (`DP102=6`) within seconds. Expected — but it
  makes most test windows short.
- **Idle chatter means it is docked.** Apparent "DP cycling" with no input (101/102/112/17
  flapping every few seconds) is charging behaviour; off the dock a 20 s baseline is near-silent
  apart from heartbeats.
- **Tray presence broadcasts nothing.** Removing/inserting the dust bin or water tank changes
  which controls the stock app shows but produces zero wire traffic.
- **Never auto-echo the heartbeat.** Replying to every received `0x00` with another `0x00` sends
  the STM32, which does something similar, into a runaway ping-pong that floods the UART as fast
  as both sides can process bytes and silently drowns out real traffic — the symptom was an app
  "Start" tap having no visible effect at all. Send periodic pings on a timer only.
- **The TX/RX labels have been mixed up twice on this project** — once on the sniffer's GPIO6/7
  and once on the bridge's solder job. Both presented as total silence in *both* directions.
  If nothing arrives, suspect the labelling before the code.

## Sniffer rig

`tools/uart_sniffer/` — passive, RX-only, never drives the bus. Two independent hardware UARTs
tap both directions at once, parse `55 AA` frames and decode DP records to friendly names;
heartbeat spam collapses to one counter line every ~20.

Built for **ESP32-C6** (a SuperMini clone), purely because that was the spare board — passive
sniffing doesn't care which chip does it. Its `sdkconfig.defaults` forces the console onto the
native USB-Serial/JTAG peripheral so UART0 *and* UART1 are both free for taps.

| Robot `J1` | Meaning | C6 pin |
|---|---|---|
| GND | ground | GND |
| RX | STM32→module | GPIO6 |
| TX | module→STM32 | GPIO7 |
| VCC | 3.3 V | **not connected** |

Avoid C6 strapping pins 4/5/8/9/15. Build with a stock ESP-IDF (v5.5.4 was used):

```powershell
& "C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1"
idf.py -p COM3 build flash monitor   # close any open monitor first; the port can't be shared
```

## Verifying secondary sources

Gemini was asked for an independent reverse-engineering as a cross-check. It cited a **real** PR
number (#2918) and **fabricated the content around it**, claiming `DP112 = charge_state` — flatly
contradicted by both the real schema and our own capture (DP112 fired on two independent Pause
taps with the device confirmed not charging). A real citation is not evidence of real content.
Fetch raw primary files; do not trust a paraphrase.
