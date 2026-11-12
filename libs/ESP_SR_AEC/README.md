# ESP_SR_AEC

Precompiled acoustic echo cancellation from Espressif's [esp-sr](https://github.com/espressif/esp-sr) (v2.6.2),
packaged as an Arduino library so the AICC Phone sketch can link `aec_create()/aec_process()`.

Contents:

- `src/esp_aec.h`, `src/esp_aec_nlp.h` – public API (from `esp-sr/include/esp32s3`)
- `src/esp32s3/libesp_audio_processor.a`, `src/esp32s3/libesp_audio_front_end.a` – prebuilt libraries (ESP32-S3 only)

License: Espressif MIT / Apache-2.0, see the esp-sr repository.

The build script passes `--library libs/ESP_SR_AEC`. For the Arduino IDE copy this folder to
`~/Documents/Arduino/libraries/ESP_SR_AEC`.
