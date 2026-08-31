# coredy-firmware

Signed OTA firmware releases for the [Coredy R750 → SmartThings bridge](https://github.com/john-mayhem/coredy-firmware) — an ESP32 that replaces the vacuum's stock Tuya WiFi module entirely, speaking the same TuyaMCU UART protocol to the STM32 on one side and a SmartThings Direct Connected Device on the other.

## How OTA works here

- Devices poll `versioninfo.json` from this repo's **latest** release.
- If its `"version"` differs from what's currently running, the device downloads the binary at the `"url"` it names.
- The last 256 bytes of that binary are an RSA-2048/SHA256 signature (PKCS#1 v1.5) over everything before it. The device verifies it against a public key embedded in firmware **before** ever writing the image to the boot partition — an unsigned, corrupted, or tampered binary is refused outright.
- The signing private key is kept **offline**, never committed here. Only someone holding it can produce a signature the device will accept.
- The bootloader's app-rollback guard is enabled: if a newly-installed image never proves itself healthy (reaches the first successful SmartThings cloud connection), the bootloader automatically reverts to the previous working image on the next boot.

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
