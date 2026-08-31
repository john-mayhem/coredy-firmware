# coredy-firmware

Firmware **source** and signed OTA **releases** for the Coredy R750 → SmartThings bridge — an ESP32 that replaces the vacuum's stock Tuya WiFi module entirely, speaking the same TuyaMCU UART protocol to the STM32 on one side and a SmartThings Direct Connected Device on the other.

- **Source**: [`firmware/`](firmware/) — see [`firmware/README.md`](firmware/README.md) for the hardware interface, build instructions, and architecture.
- **Releases**: signed binaries on the [Releases](../../releases) page.

## How OTA works here

- Devices poll `versioninfo.json` from this repo's **latest** release.
- If its `"version"` is **newer** than what's running (semver compare, not merely different), the device downloads the binary at the `"url"` it names. Republishing an older tag as latest will therefore not downgrade a fleet.
- The last 256 bytes of that binary are an RSA-2048/SHA256 signature (PKCS#1 v1.5) over everything before it. The device verifies it against a public key embedded in firmware **before** ever writing the image to the boot partition — an unsigned, corrupted, or tampered binary is refused outright.
- The signing private key is kept **offline**, never committed here. Only someone holding it can produce a signature the device will accept.

### Rollback safety

The bootloader's app-rollback guard is enabled, and a newly-installed image cancels it only
after it has **successfully fetched `versioninfo.json` itself** — not merely after it reaches
the SmartThings cloud.

That distinction is the whole point. An image that connects to SmartThings happily but cannot
fetch updates is a permanent dead end, recoverable only by physically opening the robot and
desoldering. So if a probationary image fails to reach the update server within 5 minutes of
boot, it deliberately reboots and the bootloader reverts to the last image that *could*
update itself. Failing to take an update is recoverable; losing the ability to take one is not.

## Cutting a release

```
ota/make_release.sh <version> <path-to-coredy_bridge.bin>
```

Signs the binary, appends the signature, and writes `versioninfo.json`. Then create a GitHub
Release tagged `v<version>` and upload both `coredy_bridge.signed.bin` and `versioninfo.json`
as release assets — `versioninfo.json` must be attached to whichever release is **latest**,
since that's the URL every device polls.

## Separation from other projects

This repo is intentionally separate from [`maya-firmware`](https://github.com/john-mayhem/maya-firmware) (the SmartSwitch project's OTA channel), with its own independent signing key — a leaked or rotated key on one project has no effect on the other.
