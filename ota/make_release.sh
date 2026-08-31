#!/bin/bash
# Builds a signed OTA release: signs the firmware binary with the offline
# private key, appends the signature, and writes versioninfo.json.
#
# The private key is NEVER read from inside this repo -- it must exist
# outside it, kept offline. Losing it means you can never sign an update
# again (by design); leaking it means anyone can sign a "legitimate" update
# for every device out there, so treat it accordingly.
#
# Usage: ./make_release.sh <version> <path-to-coredy_bridge.bin> [output-dir]
#   ex)  ./make_release.sh 1.0.1 ~/st-device-sdk-c-ref/apps/esp32/coredy_bridge/build/coredy_bridge.bin

set -euo pipefail

VERSION="${1:-}"
BIN="${2:-}"
OUTDIR="${3:-.}"
PRIVATE_KEY="${COREDY_OTA_PRIVATE_KEY:-$HOME/coredy-ota-keys/coredy_ota_private.pem}"
REPO_URL_BASE="https://github.com/john-mayhem/coredy-firmware/releases/download"

if [ -z "$VERSION" ] || [ -z "$BIN" ]; then
    echo "Usage: $0 <version> <path-to-coredy_bridge.bin> [output-dir]"
    exit 1
fi
if [ ! -f "$BIN" ]; then
    echo "Firmware binary not found: $BIN"
    exit 1
fi
if [ ! -f "$PRIVATE_KEY" ]; then
    echo "Private key not found at $PRIVATE_KEY -- keep it offline, never commit it to this repo."
    echo "Override its location with \$COREDY_OTA_PRIVATE_KEY if needed."
    exit 1
fi

mkdir -p "$OUTDIR"
SIG="$OUTDIR/.sig.tmp"
SIGNED="$OUTDIR/coredy_bridge.signed.bin"

# SHA256 + RSA-2048, PKCS#1 v1.5 padding (openssl's default for an RSA key --
# matches what the device verifies with mbedtls_pk_verify).
openssl dgst -sha256 -sign "$PRIVATE_KEY" -out "$SIG" "$BIN"

SIG_SIZE=$(stat -c%s "$SIG" 2>/dev/null || stat -f%z "$SIG")
if [ "$SIG_SIZE" -ne 256 ]; then
    echo "Unexpected signature size: $SIG_SIZE bytes (expected 256 for RSA-2048) -- refusing to publish"
    rm -f "$SIG"
    exit 1
fi

cat "$BIN" "$SIG" > "$SIGNED"
rm -f "$SIG"

cat > "$OUTDIR/versioninfo.json" <<EOF
{
  "version": "$VERSION",
  "url": "$REPO_URL_BASE/v$VERSION/coredy_bridge.signed.bin"
}
EOF

echo "Built:"
echo "  $SIGNED"
echo "  $OUTDIR/versioninfo.json"
echo ""
echo "Next: create a GitHub Release tagged v$VERSION and upload both files as release assets"
echo "(versioninfo.json must also be attached to the LATEST release, since devices always"
echo "poll .../releases/latest/download/versioninfo.json)."
