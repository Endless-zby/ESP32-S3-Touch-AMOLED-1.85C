#include "es7210_board.h"
#include "board_io.h"

#define ES7210_ADDR 0x40

// register map (names as in esp_codec_dev's es7210_reg.h)
#define REG00_RESET        0x00
#define REG01_CLOCK_OFF    0x01
#define REG02_MAINCLK      0x02
#define REG04_LRCK_DIVH    0x04
#define REG05_LRCK_DIVL    0x05
#define REG06_POWER_DOWN   0x06
#define REG07_OSR          0x07
#define REG08_MODE_CONFIG  0x08
#define REG09_TIME_CTRL0   0x09
#define REG0A_TIME_CTRL1   0x0A
#define REG11_SDP_IFACE1   0x11
#define REG12_SDP_IFACE2   0x12
#define REG20_ADC34_HPF2   0x20
#define REG21_ADC34_HPF1   0x21
#define REG22_ADC12_HPF2   0x22
#define REG23_ADC12_HPF1   0x23
#define REG40_ANALOG       0x40
#define REG41_MIC12_BIAS   0x41
#define REG42_MIC34_BIAS   0x42
#define REG43_MIC1_GAIN    0x43
#define REG44_MIC2_GAIN    0x44
#define REG45_MIC3_GAIN    0x45
#define REG46_MIC4_GAIN    0x46
#define REG47_MIC1_POWER   0x47
#define REG48_MIC2_POWER   0x48
#define REG49_MIC3_POWER   0x49
#define REG4A_MIC4_POWER   0x4A
#define REG4B_MIC12_POWER  0x4B
#define REG4C_MIC34_POWER  0x4C

static bool s_ok = true;
static uint8_t s_micGain = 10, s_refGain = 10;

static void w(uint8_t reg, uint8_t val) {
  if (!i2c_write_reg(ES7210_ADDR, reg, &val, 1)) { s_ok = false; Serial.printf("[ES7210] write %02x failed\n", reg); }
}
static uint8_t r(uint8_t reg) {
  uint8_t v = 0;
  if (!i2c_read_reg(ES7210_ADDR, reg, &v, 1)) s_ok = false;
  return v;
}
static void upd(uint8_t reg, uint8_t mask, uint8_t val) {
  uint8_t v = r(reg);
  v = (v & ~mask) | (val & mask);
  w(reg, v);
}

// esp_codec_dev: es7210_mic_select() with all four inputs selected
static void mic_select() {
  for (int i = 0; i < 4; i++) upd(REG43_MIC1_GAIN + i, 0x10, 0x00);
  w(REG4B_MIC12_POWER, 0xff);
  w(REG4C_MIC34_POWER, 0xff);
  // MIC1
  upd(REG01_CLOCK_OFF, 0x0b, 0x00); w(REG4B_MIC12_POWER, 0x00);
  upd(REG43_MIC1_GAIN, 0x10, 0x10); upd(REG43_MIC1_GAIN, 0x0f, s_micGain);
  // MIC2
  upd(REG01_CLOCK_OFF, 0x0b, 0x00); w(REG4B_MIC12_POWER, 0x00);
  upd(REG44_MIC2_GAIN, 0x10, 0x10); upd(REG44_MIC2_GAIN, 0x0f, s_micGain);
  // MIC3 (DAC loopback = echo reference on this board)
  upd(REG01_CLOCK_OFF, 0x15, 0x00); w(REG4C_MIC34_POWER, 0x00);
  upd(REG45_MIC3_GAIN, 0x10, 0x10); upd(REG45_MIC3_GAIN, 0x0f, s_refGain);
  // MIC4 (unused)
  upd(REG01_CLOCK_OFF, 0x15, 0x00); w(REG4C_MIC34_POWER, 0x00);
  upd(REG46_MIC4_GAIN, 0x10, 0x10); upd(REG46_MIC4_GAIN, 0x0f, s_micGain);
  // four channels -> 1xFS TDM
  w(REG12_SDP_IFACE2, 0x02);
}

bool es7210_board_init(uint32_t sampleRate, uint8_t micGainCode, uint8_t refGainCode) {
  s_ok = true;
  s_micGain = micGainCode > 14 ? 14 : micGainCode;
  s_refGain = refGainCode > 14 ? 14 : refGainCode;
  if (sampleRate != 16000) { Serial.println("[ES7210] only 16 kHz is supported"); return false; }

  // ---- es7210_open() ----
  w(REG00_RESET, 0xff);
  w(REG00_RESET, 0x41);
  w(REG01_CLOCK_OFF, 0x3f);
  w(REG09_TIME_CTRL0, 0x30);
  w(REG0A_TIME_CTRL1, 0x30);
  w(REG23_ADC12_HPF1, 0x2a);
  w(REG22_ADC12_HPF2, 0x0a);
  w(REG20_ADC34_HPF2, 0x0a);
  w(REG21_ADC34_HPF1, 0x2a);
  upd(REG08_MODE_CONFIG, 0x01, 0x00);       // slave
  w(REG40_ANALOG, 0x43);
  w(REG41_MIC12_BIAS, 0x70);                // 2.87 V
  w(REG42_MIC34_BIAS, 0x70);
  w(REG07_OSR, 0x20);
  w(REG02_MAINCLK, 0xc1);
  mic_select();
  uint8_t offReg = r(REG01_CLOCK_OFF);      // 0x20 after the selects above

  // ---- es7210_set_fs(): 16-bit words, 16 kHz with MCLK = 256 fs = 4.096 MHz, I2S format ----
  upd(REG11_SDP_IFACE1, 0xe0, 0x60);        // 16-bit
  w(REG02_MAINCLK, 0x01 | (0x01 << 6) | (0x01 << 7));   // adc_div=1, doubler, dll
  w(REG07_OSR, 0x20);
  w(REG04_LRCK_DIVH, 0x01);
  w(REG05_LRCK_DIVL, 0x00);
  upd(REG11_SDP_IFACE1, 0x03, 0x00);        // standard I2S

  // ---- es7210_start() ----
  w(REG01_CLOCK_OFF, offReg);
  w(REG06_POWER_DOWN, 0x00);
  w(REG40_ANALOG, 0x43);
  w(REG47_MIC1_POWER, 0x08);
  w(REG48_MIC2_POWER, 0x08);
  w(REG49_MIC3_POWER, 0x08);
  w(REG4A_MIC4_POWER, 0x08);
  mic_select();
  w(REG40_ANALOG, 0x43);
  w(REG00_RESET, 0x71);
  w(REG00_RESET, 0x41);

  Serial.printf("[ES7210] init %s (REG01=0x%02x REG11=0x%02x REG12=0x%02x REG4B=0x%02x REG4C=0x%02x)\n",
                s_ok ? "ok" : "FAILED", r(REG01_CLOCK_OFF), r(REG11_SDP_IFACE1), r(REG12_SDP_IFACE2),
                r(REG4B_MIC12_POWER), r(REG4C_MIC34_POWER));
  return s_ok;
}

bool es7210_board_set_gain(uint8_t mic, uint8_t gainCode) {
  if (mic < 1 || mic > 4) return false;
  s_ok = true;
  upd(REG43_MIC1_GAIN + (mic - 1), 0x0f, gainCode > 14 ? 14 : gainCode);
  return s_ok;
}

