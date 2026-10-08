#!/bin/sh
# Downloads the ESP8266 mask ROM for a CI build:  sh .github/get_rom.sh <out file>
#
# From $ESP8266_ROM_URL (a repository secret or variable, see build.yml), and
# only kept when it is the 64 KB ROM: its SHA-256 must be $ESP8266_ROM_SHA256.
# A wrong file stops the build rather than ending up in a release.
set -e
OUT=${1:-bios/esp8266_rom.bin}
if [ -z "$ESP8266_ROM_URL" ]; then
    echo "ESP8266_ROM_URL is not set: building without the ROM"
    exit 0
fi
mkdir -p "$(dirname "$OUT")"
curl -fsSL --retry 3 -o "$OUT.tmp" "$ESP8266_ROM_URL"
SIZE=$(wc -c < "$OUT.tmp" | tr -d ' ')
if [ "$SIZE" != 65536 ]; then
    echo "the file at ESP8266_ROM_URL is $SIZE bytes, not the 64 KB ROM" >&2
    rm -f "$OUT.tmp"
    exit 1
fi
if command -v sha256sum > /dev/null 2>&1; then
    SUM=$(sha256sum "$OUT.tmp" | cut -d' ' -f1)
else
    SUM=$(shasum -a 256 "$OUT.tmp" | cut -d' ' -f1)     # macOS
fi
if [ -n "$ESP8266_ROM_SHA256" ] && [ "$SUM" != "$ESP8266_ROM_SHA256" ]; then
    echo "the file at ESP8266_ROM_URL has SHA-256 $SUM, not the ESP8266 ROM's $ESP8266_ROM_SHA256" >&2
    rm -f "$OUT.tmp"
    exit 1
fi
mv "$OUT.tmp" "$OUT"
echo "ESP8266 ROM downloaded to $OUT ($SIZE bytes, SHA-256 checked)"
