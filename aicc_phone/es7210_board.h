#pragma once
#include <Arduino.h>

// ES7210 four-channel ADC on the V2 board, programmed with the exact register
// sequence of Espressif's esp_codec_dev driver (the one xiaozhi-esp32 uses on
// this board): slave mode, 1xFS TDM, 16-bit words, all four inputs enabled.
//
// Gain codes: 0=0dB 1=3 2=6 3=9 4=12 5=15 6=18 7=21 8=24 9=27 10=30 11=33
//             12=34.5 13=36 14=37.5 dB
bool es7210_board_init(uint32_t sampleRate, uint8_t micGainCode, uint8_t refGainCode);
bool es7210_board_set_gain(uint8_t mic /*1..4*/, uint8_t gainCode);
void es7210_board_dump();   // print the register map to Serial
