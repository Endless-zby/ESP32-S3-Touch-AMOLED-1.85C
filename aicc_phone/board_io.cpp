#include "board_io.h"
#include "config.h"
#include <Wire.h>

// ---------------------------------------------------------------------------
// I2C
// ---------------------------------------------------------------------------
void board_i2c_init() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  Wire.setTimeOut(50);
}

bool i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  size_t got = Wire.requestFrom((int)addr, (int)len);
  for (size_t i = 0; i < len; i++) data[i] = (i < got) ? Wire.read() : 0;
  return got == len;
}

bool i2c_write_reg(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  for (size_t i = 0; i < len; i++) Wire.write(data[i]);
  return Wire.endTransmission(true) == 0;
}

// ---------------------------------------------------------------------------
// TCA9554PWR
// ---------------------------------------------------------------------------
#define TCA9554_ADDR 0x20
#define TCA9554_REG_IN 0x00
#define TCA9554_REG_OUT 0x01
#define TCA9554_REG_CFG 0x03
static uint8_t s_exio_out = 0;

void exio_init(uint8_t initialOutput) {
  s_exio_out = initialOutput;
  uint8_t cfg = 0x00;  // all outputs
  i2c_write_reg(TCA9554_ADDR, TCA9554_REG_OUT, &s_exio_out, 1);
  i2c_write_reg(TCA9554_ADDR, TCA9554_REG_CFG, &cfg, 1);
}

void exio_set(uint8_t pin, bool level) {
  if (pin < 1 || pin > 8) return;
  uint8_t mask = 1 << (pin - 1);
  if (level) s_exio_out |= mask; else s_exio_out &= ~mask;
  i2c_write_reg(TCA9554_ADDR, TCA9554_REG_OUT, &s_exio_out, 1);
}

// ---------------------------------------------------------------------------
// Backlight (LEDC PWM)
// ---------------------------------------------------------------------------
void backlight_init() {
  ledcAttach(PIN_LCD_BL, 20000, 10);
  ledcWrite(PIN_LCD_BL, 0);
}

void backlight_set(uint8_t percent) {
  if (percent > 100) percent = 100;
  uint32_t duty = (uint32_t)percent * 1023 / 100;
  ledcWrite(PIN_LCD_BL, duty);
}

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------
void battery_init() {
  analogReadResolution(12);
  pinMode(PIN_BAT_ADC, INPUT);
}

float battery_volts() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BAT_ADC);
  mv /= 8;
  // divider 1:3 on the board (per Waveshare demo: *3 and small offset)
  return (mv / 1000.0f) * 3.0f / 0.990476f;
}
