#ifndef _COREDY_OTA_
#define _COREDY_OTA_

// Signed OTA client for the Coredy R750 bridge. Polls
// https://github.com/john-mayhem/coredy-firmware releases for a
// versioninfo.json naming a newer version + download URL, downloads the
// signed binary, verifies its RSA-2048/SHA256 signature (appended as the
// last 256 bytes of the file) against the embedded public key, and only
// then stages it as the boot partition and reboots. An unsigned or
// corrupted image is refused outright -- never written to the boot
// partition. Paired with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE so a
// signed-but-buggy image that crashes on boot gets auto-reverted by the
// bootloader if coredy_ota_mark_valid() is never reached.

#ifdef __cplusplus
extern "C" {
#endif

// Bump this on every release -- compared as a plain string against
// versioninfo.json's "version" field (no semver parsing, exact match only).
#define COREDY_FW_VERSION "1.0.4"

// Starts the background task that checks for updates every 30 seconds,
// continuously, for as long as the device is powered. No WiFi/cloud gating
// -- a check just fails harmlessly (logged, not fatal) if there's no
// connection yet, and the next 30s tick tries again.
void coredy_ota_start(void);

// Call once the app has proven itself healthy (first successful cloud
// connect) to cancel the bootloader's rollback-on-next-boot-failure guard.
// Idempotent -- safe to call more than once.
void coredy_ota_mark_valid(void);

#ifdef __cplusplus
}
#endif

#endif
