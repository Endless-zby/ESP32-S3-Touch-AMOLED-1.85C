#!/usr/bin/env bash
# Flash the firmware. Usage: tools/flash.sh [serial-port]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DIRECTORIES_DATA:-$HOME/Library/Arduino15}"
export ARDUINO_DIRECTORIES_USER="${ARDUINO_DIRECTORIES_USER:-$HOME/Documents/Arduino}"
FQBN="esp32:esp32:waveshare_esp32_s3_touch_lcd_185:PSRAM=enabled,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,UploadSpeed=921600"
PORT="${1:-}"
if [ -z "$PORT" ]; then
  PORT="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -1 || true)"
fi
if [ -z "$PORT" ]; then
  echo "No serial port found. Plug in the board (hold BOOT while pressing RESET if it is not detected)." >&2
  exit 1
fi
echo "Flashing via $PORT"
"$CLI" upload --fqbn "$FQBN" --input-dir "$ROOT/build" -p "$PORT" "$ROOT/aicc_phone"
