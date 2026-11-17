#pragma once
#include <Arduino.h>

void board_i2c_init();
bool i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
bool i2c_write_reg(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);

// TCA9554 IO expander (pins 1..8, all configured as outputs)
void exio_init(uint8_t initialOutput = 0x00);
void exio_set(uint8_t pin, bool level);

void backlight_init();
void backlight_set(uint8_t percent);   // 0..100

void battery_init();
float battery_volts();
