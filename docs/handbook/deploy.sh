#!/usr/bin/env bash
# Publish docs/handbook/device-phone.html to the handbook site (ssh alias "vm", /opt/handbook, port 18090)
# and add the "客房话机" nav entry + cards. Idempotent: safe to re-run after editing the page.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
scp -q "$HERE/device-phone.html" vm:/opt/handbook/device-phone.html
ssh vm 'set -e; cd /opt/handbook
  stamp=$(date +%Y%m%d-%H%M%S); mkdir -p ".bak-$stamp"; cp *.html ".bak-$stamp/"
  # nav entry after the 语音伴侣设备 link on every page that carries the shared nav
  for f in index.html dsl.html webhook.html outbound.html turn.html u1960.html voices.html notes.html devices.html; do
    grep -q "device-phone.html" "$f" && continue
    sed -i "/<a href=\"devices.html\"[^>]*>语音伴侣设备<\/a>/a\\      <a href=\"device-phone.html\">客房话机</a>" "$f"
  done
  # card on the devices hub
  if ! grep -q "device-phone.html" devices.html; then :; fi
  grep -q "class=\"card\" href=\"device-phone.html\"" devices.html || sed -i "s#<div class=\"cards\">#<div class=\"cards\">\n    <a class=\"card\" href=\"device-phone.html\">\n      <span class=\"tag\">新设备 · 话机</span>\n      <h3>1.85″ 圆屏话机 · ESP32-S3</h3>\n      <p>微雪 ESP32-S3-Touch-LCD-1.85C，360×360 触摸圆屏，双麦 + 硬件回声参考 + 5 W 喇叭。走 SIP 线路直呼 AI 前台，喊“小西小西，呼叫前台”即拨号。使用手册、语音指令表、扩展规划。</p>\n    </a>#" devices.html
  # card on the home page, right after the 语音伴侣 card
  grep -q "class=\"card\" href=\"device-phone.html\"" index.html || sed -i "/<a class=\"card\" href=\"devices.html\">/a\\    <a class=\"card\" href=\"device-phone.html\"><span class=\"tag\">设备</span><h3>客房 AI 前台话机「小西话机」</h3><p>1.85″ 圆屏 SIP 话机：语音唤醒“小西小西，呼叫前台”一键接通 AI 前台，免提全双工。使用手册、语音指令、后续扩展。</p></a>" index.html
  # lead sentence on the devices hub
  grep -q "话机" devices.html | head -1 >/dev/null || true
  sed -i "s#两代硬件，同一套语音服务与交互。</p>#两代硬件，同一套语音服务与交互。另有一台功能完全不同的<a href=\"device-phone.html\">客房 AI 前台话机</a>：不聊天，直接把客房电话打到 AI 前台。</p>#" devices.html
  echo "---- verify"
  for f in index.html devices.html dsl.html notes.html; do echo "$f: $(grep -c device-phone.html $f) refs"; done
  curl -s -o /dev/null -w "device-phone.html -> HTTP %{http_code}, %{size_download} bytes\n" http://127.0.0.1:18090/device-phone.html
  curl -s http://127.0.0.1:18090/index.html | grep -o "device-phone.html\">[^<]*" | head -2'
