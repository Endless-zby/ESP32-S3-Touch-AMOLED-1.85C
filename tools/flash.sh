#!/usr/bin/env bash
# Flash the firmware (application + partition table).
# Usage: tools/flash.sh [serial-port] [--models]
#   --models  also write tools/srmodels/srmodels_cn.bin into the 4 MB "model"
#             partition (offset 0x410000 of aicc_phone/partitions.csv).
#             Only needed when the model pack / command table changes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DIRECTORIES_DATA:-$HOME/Library/Arduino15}"
export ARDUINO_DIRECTORIES_USER="${ARDUINO_DIRECTORIES_USER:-$HOME/Documents/Arduino}"
FQBN="esp32:esp32:waveshare_esp32_s3_touch_lcd_185:PSRAM=enabled,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,UploadSpeed=921600"
MODELS=0
PORT=""
for a in "$@"; do
  case "$a" in
    --models) MODELS=1 ;;
    *) PORT="$a" ;;
  esac
done
if [ -z "$PORT" ]; then
  PORT="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -1 || true)"
fi
if [ -z "$PORT" ]; then
  echo "No serial port found. Plug in the board (hold BOOT while pressing RESET if it is not detected)." >&2
  exit 1
fi
echo "Flashing via $PORT"
"$CLI" upload --fqbn "$FQBN" --input-dir "$ROOT/build" -p "$PORT" "$ROOT/aicc_phone"

if [ "$MODELS" = 1 ]; then
  MODEL_BIN="$ROOT/tools/srmodels/srmodels_cn.bin"
  ESPTOOL="$(ls ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | head -1 || true)"
  if [ -z "$ESPTOOL" ] || [ ! -f "$MODEL_BIN" ]; then
    echo "Model flash skipped (esptool or $MODEL_BIN missing)." >&2
    exit 1
  fi
  sleep 1
  PORT="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -1 || true)"
  echo "Writing model pack to $PORT @0x410000"
  "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 write-flash 0x410000 "$MODEL_BIN"
fi
