#!/usr/bin/env bash
# Flash the prebuilt images in build/ onto a Waveshare ESP32-S3-Touch-LCD-1.85C
# using only esptool (no Arduino IDE / arduino-cli needed).
#
# Usage: tools/flash_release.sh [serial-port] [--models] [--erase] [--baud N]
#   --models  also write tools/srmodels/srmodels_cn.bin (wake-word model pack)
#             into the "model" partition @0x410000.  REQUIRED for a brand-new
#             device, otherwise "你好小安" cannot be recognised.
#   --erase   erase the whole flash first (clears Wi-Fi/SIP/API-key settings).
#   --baud N  default 921600; use 460800 if writes fail on a long/cheap cable.
#
# Images and offsets (from build/flash_args, partition table aicc_phone/partitions.csv):
#   0x0       aicc_phone.ino.bootloader.bin
#   0x8000    aicc_phone.ino.partitions.bin
#   0xe000    boot_app0.bin
#   0x10000   aicc_phone.ino.bin            (application, ~2.4 MB)
#   0x410000  tools/srmodels/srmodels_cn.bin (model pack, --models)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build"
MODEL_BIN="$ROOT/tools/srmodels/srmodels_cn.bin"

PORT=""; MODELS=0; ERASE=0; BAUD=921600
while [ $# -gt 0 ]; do
  case "$1" in
    --models|--all) MODELS=1 ;;
    --erase) ERASE=1 ;;
    --baud) shift; BAUD="$1" ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) PORT="$1" ;;
  esac
  shift
done

# ---- find esptool -----------------------------------------------------------
find_esptool() {
  if [ -n "${ESPTOOL:-}" ]; then echo "$ESPTOOL"; return; fi
  if command -v esptool >/dev/null 2>&1; then echo "esptool"; return; fi
  if command -v esptool.py >/dev/null 2>&1; then echo "esptool.py"; return; fi
  if python3 -c "import esptool" >/dev/null 2>&1; then echo "python3 -m esptool"; return; fi
  local a
  for a in "$HOME/Library/Arduino15" "$HOME/.arduino15"; do
    local f
    f="$(ls "$a"/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | head -1 || true)"
    [ -n "$f" ] && { echo "$f"; return; }
  done
  echo ""
}
ESPTOOL_CMD="$(find_esptool)"
if [ -z "$ESPTOOL_CMD" ]; then
  echo "esptool not found. Install it with:  pip3 install esptool   (or set ESPTOOL=/path/to/esptool)" >&2
  exit 1
fi

# ---- find the serial port ---------------------------------------------------
if [ -z "$PORT" ]; then
  PORT="$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1 || true)"
fi
if [ -z "$PORT" ]; then
  echo "No serial port found. Plug in the board; if it is still not detected hold BOOT, tap RESET, release BOOT." >&2
  exit 1
fi

# ---- sanity ------------------------------------------------------------------
for f in aicc_phone.ino.bootloader.bin aicc_phone.ino.partitions.bin boot_app0.bin aicc_phone.ino.bin; do
  [ -f "$B/$f" ] || { echo "Missing $B/$f (run tools/build.sh or git pull)." >&2; exit 1; }
done
if [ "$MODELS" = 1 ] && [ ! -f "$MODEL_BIN" ]; then
  echo "Missing $MODEL_BIN" >&2; exit 1
fi

COMMON=(--chip esp32s3 --port "$PORT" --baud "$BAUD" --before default_reset --after hard_reset)
FLASHARGS=(--flash_mode dio --flash_freq 80m --flash_size 16MB)

echo "esptool : $ESPTOOL_CMD"
echo "port    : $PORT"
echo "app     : $(ls -la "$B/aicc_phone.ino.bin" | awk '{print $5}') bytes"
if [ "$ERASE" = 1 ]; then
  echo "== erase_flash =="
  $ESPTOOL_CMD "${COMMON[@]}" erase_flash
fi

echo "== write firmware =="
$ESPTOOL_CMD "${COMMON[@]}" write_flash "${FLASHARGS[@]}" \
  0x0      "$B/aicc_phone.ino.bootloader.bin" \
  0x8000   "$B/aicc_phone.ino.partitions.bin" \
  0xe000   "$B/boot_app0.bin" \
  0x10000  "$B/aicc_phone.ino.bin"

if [ "$MODELS" = 1 ]; then
  sleep 1
  echo "== write wake-word model pack @0x410000 =="
  $ESPTOOL_CMD "${COMMON[@]}" write_flash "${FLASHARGS[@]}" 0x410000 "$MODEL_BIN"
fi
echo "Done. The board resets by itself; open the serial log with: python3 tools/serial_log.py $PORT 60 /tmp/boot.log"
