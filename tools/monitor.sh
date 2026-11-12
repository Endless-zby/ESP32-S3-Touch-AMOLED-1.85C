#!/usr/bin/env bash
# Serial monitor. Usage: tools/monitor.sh [serial-port]
set -euo pipefail
CLI="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DIRECTORIES_DATA:-$HOME/Library/Arduino15}"
PORT="${1:-$(ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1)}"
"$CLI" monitor -p "$PORT" --config baudrate=115200
