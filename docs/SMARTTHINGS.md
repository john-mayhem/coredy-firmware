# SmartThings side — device profile and capabilities

How the robot appears in SmartThings, and the Developer Workspace / CLI behaviour that shapes
it. The route chosen is **STDK direct-connected**, not Matter. (Noted only in case that is ever
revisited: SmartThings' Matter RVC driver lacks a `GoHome` mapping — Matter 1.2 predates it.)

The profile was designed and validated *before* the bridge existed, using a dummy device on a
spare ESP32 with hardcoded values, so the app rendering could be settled without risking the
robot. Every enum invented in that exercise later matched real hardware — including
`waterLevel`, which was pure speculation at the time and turned out to be [DP120](PROTOCOL.md).

## Device profile

- Device type `RobotCleaner` (OCF `oic.d.robotcleaner`)
- `metadata.mainState` = `robotCleanerOperatingState` (drives the detail-view hero card)
- `metadata.mainAction` = custom `cleaningMode` (drives the dashboard quick-action button) —
  two independent settings, easy to conflate.

The account-specific values — `mnId`, `mnmn`, profile id, `vid`, capability namespace — are
assigned by the Developer Workspace and live in `main/onboarding_config.json`, which is
gitignored. This doc writes the custom namespace as `ns.`.

Device identity is generated with STDK keygen and registered as a Test Device:

```bash
iot-core/tools/keygen/stdk-keygen.py --mnid <mnId> --firmware <label> --path <app>/main/ --qr
# needs a venv: pynacl, qrcode, pillow
```

## Capability map

| Capability | Backing DP | Choice |
|---|---|---|
| `battery` | DP108 | stock, clean 1:1 |
| `robotCleanerOperatingState` | DP112 + DP101 | stock capability, but no pregenerated STDK helper — hand-written |
| `momentary` (Find Me) | DP116 | stock, exact fit |
| `ns.cleaningMode` — auto/edge/spot/smallRoom/home | DP102 | **custom** — stock `robotCleanerCleaningMode`'s enum doesn't match the real modes |
| `ns.suctionLevel` — quiet/normal/max | DP104 | **custom** — stock `robotCleanerTurboMode` enum doesn't match either |
| `ns.waterLevel` — low/medium/high | DP120 | **custom** — no stock equivalent |
| `ns.hardwareFault` — `summary` string | DP17 | **custom** — stock `hardwareFault` is a bare clear/detected flag and loses *which* faults fired |
| `ns.areaCleaned` — number, `m^2` | DP14 | **custom** — no stock capability tracks cleaned area |
| `ns.consumables` — brush / rollerBrush / filter %, all on one capability | DP109/110/111 | **custom**, for UI reasons — see the multi-`detailView` note below |

Stock `robotCleanerCleaningMode` and `robotCleanerTurboMode`
are both listed by SmartThings as **"proposed"** status, which was part of the reason to stop
fighting their enums and go custom.

Dropped deliberately: `robotCleanerMovement` and `batteryLevel` — no independent DP backing,
redundant with OperatingState and `battery`.

DP17 is a 9-bit field but is published as friendly text (`"None"`, or comma-joined names)
because **array attributes have no friendly rendering in the app at all** — a `{{attr.value}}`
template on an array dumps raw JSON.

## Workspace / CLI gotchas

Each of these cost real time.

**Capabilities**
- **A capability existing in a profile does not create it.** Referencing `namespace.capName`
  before `smartthings capabilities:create` throws `Capability does not exist`. Profile JSON
  cannot create capabilities.
- **Create and update take different shapes.** Create input is just `{name, attributes, commands}`
  and may omit `commands.<cmd>.name` (auto-filled). **Update validates strictly and requires it**,
  or 422s with a confusing "Command name cannot be null".
- **The base `name` is immutable.** `capabilities:update` accepts a changed `name` with 200 OK
  and silently no-ops it. This matters because **the app renders the base `name` as the section
  header**, not `detailView[].label` — so renaming via presentation alone changes nothing
  visible. Only a genuinely new capability fixes a wrong display name. (This is why
  `turboMode` → `suctionLevel` is a new capability, not a rename.)
- **Attribute schemas, unlike names, *are* updatable in place** — used to swap `hardwareFault`'s
  array attribute for a plain string without a new capability id.

**Presentations** — display types silently drop fields they don't recognise rather than erroring
- `list` (enum picker) needs `list.command.alternatives` **and** a mirrored
  `list.state.alternatives` + `state.value`/`state.valueType`. Omitting `state.alternatives`
  gives a precise 422; getting a *field name* wrong is silently dropped — blank display, no error.
- `state` (read-only display) needs **`label`** (a `{{attr.value}}` template), *not*
  `value`/`valueType` like `list.state` uses.
- `numberField` **requires a command** — it is an editable control, not a read-only widget,
  despite being listed generically under "Detail View". Use `slider` for a read-only numeric
  gauge (the battery look); it works fine with no command.
- **One capability's presentation can hold multiple `detailView` entries**, each targeting a
  different attribute of that same capability. That is how three consumable percentages became
  three rows in one card instead of three components.
- When guessing, pull a stock capability's real presentation as reference (`tamperAlert` for
  state+alternatives, `battery`/`powerMeter` for slider) rather than iterating blind.

**Profiles and pairing**
- Edit with `smartthings deviceprofiles:view:update <id> -i file.json` — *not* plain
  `deviceprofiles:update` (device-profile-only), and not by pasting exported JSON into the
  Workspace UI. Its input shape is flat (`name`/`metadata`/`components` at top level) with
  dashboard/detailView/automation under a **`view`** key — not `deviceConfiguration`, which is
  what the Workspace UI exports.
- **Every view update that changes the capability or component list mints a new `vid`**, and a
  paired device never picks it up live — force-closing the app doesn't help. The reliable cycle
  is: new `vid` into `onboarding_config.json` → rebuild → **`esptool erase-flash`** (stale NVS
  provisioning fights the new pairing; overwriting is not enough) → reflash → delete the device
  in the app → re-add while the board re-advertises over BLE. This ran ~5 times in one session.
- Multi-component devices get a pencil-icon rename label per section in the app; it disappears
  when back to a single component.
- The stock `battery` capability always pins a battery-% badge in the page header. Fixed
  platform behaviour, not configurable.

**CLI install**: `@smartthings/cli` v1.10.6 fails on Windows with
`Error: Invalid base URL undefined` on every command (known config-format bug across major
versions). Fix: `npm install -g @smartthings/cli@latest` then `smartthings logout` to reset the
config.

## Widgets

Nothing to implement. Home-screen widget support is entirely a user-side action: mark the device
Favorite → add the general SmartThings widget → enable it in widget settings. No profile-side
config. (The Samsung-TV "SmartThings Remote" widget is unrelated and has its own restrictions.)

## STDK notes

- **STDK does not support the ESP32-C6.** Every branch of `st-device-sdk-c-ref`, `main` included,
  pins `bsp/esp32` to ESP-IDF **v5.0.7** (`e5617c26f7f`); C6 support landed in IDF v5.1. No
  community fork adds it. `setup.py`/`build.py` officially support `esp32`, `esp32c3`, `esp32s3`.
  This is why the bridge is classic ESP32 rather than the originally-planned S3.
- `python setup.py esp32c3` installs the **full** toolchain set for every target IDF v5.0.7
  supports, so `xtensa-esp32-elf` for classic ESP32 comes along automatically.
- On Ubuntu 24.04, `setup.py` shells out to a bare `python`; install `python-is-python3`.
- Clone `--recursive` but skip the `bsp/bk7236` submodule — it points at a private GitLab repo
  and is not needed for ESP32.
- `ST_CAP_SEND_ATTR_STRING` / `_NUMBER` / `_STRINGS_ARRAY` (`st_dev.h`) are generic over any
  capability id and attribute name — no dependency on the SDK "knowing" a capability, which is
  what makes hand-rolling custom-capability C straightforward. `apps/capability_sample/` has
  usable structural templates: `battery`, `robotCleanerMovement`, `robotCleanerTurboMode`,
  `contactSensor` (read-only enum) and `ovenOperatingState` (multi-attribute with commands).
