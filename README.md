# coredy-firmware

An ESP32 that **replaces** the Coredy R750 robot vacuum's stock Tuya Wi-Fi module — speaking the
same TuyaMCU serial protocol to the robot's STM32 on one side, and SmartThings as a Direct
Connected Device on the other. No Tuya cloud is involved anywhere.

The R750 is a 2018 Tuya white-label vacuum with no official SmartThings integration. The stock
`C200-W` Wi-Fi daughterboard is unplugged and an ESP32 takes its place on the same four-pin
header.

**Status**: sealed inside the robot and running. Full command set (Start / Pause / GoHome /
suction / cleaning mode / water level / Find Me) verified live, nine SmartThings capabilities
publishing, unattended signed OTA working through real dock and charge cycles. Current release
**v1.4.0**.

## Documentation

| | |
|---|---|
| [`docs/PROTOCOL.md`](docs/PROTOCOL.md) | Robot hardware, TuyaMCU wire format, the full DP map, behavioural quirks, sniffer rig |
| [`docs/SMARTTHINGS.md`](docs/SMARTTHINGS.md) | Device profile, capability mapping, Developer Workspace and CLI gotchas |
| [`docs/OPERATIONS.md`](docs/OPERATIONS.md) | Build environment, flashing and recovery, cutting a release, on-device endpoints, field diagnostics |
| [`firmware/README.md`](firmware/README.md) | Source-tree architecture and the hardware interface |
| [`CHANGELOG.md`](CHANGELOG.md) | Release history |

## Repo layout

```
firmware/coredy_bridge/   the bridge firmware (classic ESP32, STDK 2.3.2)
tools/uart_sniffer/       passive TuyaMCU sniffer used to derive the DP map (ESP32-C6)
ota/make_release.sh       signs and packages a release binary
docs/                     everything above
```

## How OTA works here

- Devices poll `versioninfo.json` from this repo's **latest** release, every 30 s.
- If its `"version"` is **newer** than what is running (semver compare, not merely different),
  the device downloads the binary at the `"url"` it names. Republishing an older tag as latest
  therefore will not downgrade a fleet.
- The last 256 bytes of that binary are an RSA-2048/SHA-256 (PKCS#1 v1.5) signature over
  everything before it. The device verifies it against a public key embedded in firmware
  **before** writing anything to the boot partition — an unsigned, corrupted or tampered binary
  is refused outright.
- The signing private key is kept offline and never committed here. Only someone holding it can
  produce a signature the device will accept.

The 30 s interval is continuous and deliberate, not a placeholder oversight — a temporary
project decision to be revisited in a later update.

### Rollback safety

The bootloader's app-rollback guard is enabled, and a newly-installed image cancels it only
after it has **successfully fetched `versioninfo.json` itself** — not merely after it reaches the
SmartThings cloud.

That distinction is the whole point. An image that connects to SmartThings happily but cannot
fetch updates is a permanent dead end, recoverable only by physically opening the robot and
desoldering — which is exactly what happened once. So a probationary image that fails to reach
the update server within 5 minutes of boot deliberately reboots, and the bootloader reverts to
the last image that *could* update itself. Failing to take an update is recoverable; losing the
ability to take one is not.

See [`docs/OPERATIONS.md`](docs/OPERATIONS.md) for the release procedure.

## Open items

1. **Confirm DP120 = 0.** Select `low` in the app *while the robot is running*, or
   `GET :8080/dp120?value=0`. An echo of `0` settles the last inferred value in the entire DP map.
2. **DP101 `charged` and `cleaning_complete`** — the two remaining unobserved enum values. Both
   need a full charge on the dock and a clean run that actually finishes; they surface in the log
   as `DP101=N is an unconfirmed state`.
3. **DHCP reservation** for `a4:f0:0f:5b:54:84`. Sealed, the log viewer and the `:8080` API are
   the only window in, and a stable address is worth having.
4. **Bootloader headroom** is 2112 bytes; secure boot or flash encryption will not fit as-is.

## Separation from other projects

Intentionally separate from [`maya-firmware`](https://github.com/john-mayhem/maya-firmware) (the
SmartSwitch project's OTA channel), with its own independent signing key — a leaked or rotated
key on one has no effect on the other.
