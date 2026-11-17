Copy of arduinoWebSockets 2.7.2 (Links2004) with one change:
`WEBSOCKETS_MAX_DATA_SIZE` raised from 15 KB to 256 KB in `src/WebSockets.h`.
The library closes the connection with code 1009 when a single frame exceeds the
limit; DashScope Realtime sends `response.audio.delta` frames well above 15 KB.
Payload buffers are malloc'ed and land in PSRAM on this board.
tools/build.sh links this copy with `--library`; for the Arduino IDE replace
~/Documents/Arduino/libraries/WebSockets with this folder.

Second change: frame payload buffers are allocated from PSRAM (heap_caps_malloc MALLOC_CAP_SPIRAM) on ESP32 builds with PSRAM.
