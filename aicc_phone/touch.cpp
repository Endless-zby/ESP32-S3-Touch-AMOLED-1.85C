#include "touch.h"
#include "config.h"
#include "board_io.h"

#define CST816_ADDR 0x15
#define CST816_REG_GESTURE 0x01
#define CST816_REG_CHIPID 0xA7
#define CST816_REG_DIS_AUTOSLEEP 0xFE

static bool s_ok = false;

bool touch_init() {
  exio_set(EXIO_TP_RST, false);
  vTaskDelay(pdMS_TO_TICKS(10));
  exio_set(EXIO_TP_RST, true);
  vTaskDelay(pdMS_TO_TICKS(60));
  pinMode(PIN_TP_INT, INPUT_PULLUP);

  uint8_t id[3] = {0};
  s_ok = i2c_read_reg(CST816_ADDR, CST816_REG_CHIPID, id, 3);
  Serial.printf("[TP] CST816 chip=0x%02x proj=0x%02x fw=0x%02x ok=%d\n", id[0], id[1], id[2], s_ok);
  uint8_t dis = 0x01;  // disable auto sleep so polling keeps working
  i2c_write_reg(CST816_ADDR, CST816_REG_DIS_AUTOSLEEP, &dis, 1);
  return s_ok;
}

bool touch_read(uint16_t &x, uint16_t &y) {
  uint8_t buf[6];
  if (!i2c_read_reg(CST816_ADDR, CST816_REG_GESTURE, buf, 6)) return false;
  uint8_t points = buf[1] & 0x0F;
  if (points == 0) return false;
  x = ((buf[2] & 0x0F) << 8) | buf[3];
  y = ((buf[4] & 0x0F) << 8) | buf[5];
  if (x >= LCD_W) x = LCD_W - 1;
  if (y >= LCD_H) y = LCD_H - 1;
  return true;
}
