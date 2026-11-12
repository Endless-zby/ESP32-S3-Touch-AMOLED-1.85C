#!/usr/bin/env bash
# Compile the firmware with the arduino-cli bundled in Arduino IDE 2.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DIRECTORIES_DATA:-$HOME/Library/Arduino15}"
export ARDUINO_DIRECTORIES_USER="${ARDUINO_DIRECTORIES_USER:-$HOME/Documents/Arduino}"
FQBN="esp32:esp32:waveshare_esp32_s3_touch_lcd_185:PSRAM=enabled,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,UploadSpeed=921600"
OUT="$ROOT/build"
mkdir -p "$OUT"
"$CLI" compile --fqbn "$FQBN" --build-path "$OUT" --library "$ROOT/libs/ESP_SR_AEC" "$ROOT/aicc_phone" "$@"
echo
echo "Binaries: $OUT/aicc_phone.ino.bin  (merged: $OUT/aicc_phone.ino.merged.bin)"
