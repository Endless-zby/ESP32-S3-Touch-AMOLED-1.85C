#include "display.h"
#include "config.h"
#include "board_io.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_st77916.h"

#define LCD_OPCODE_READ_CMD (0x0BULL)

// Alternative init sequence used by one of the two panel suppliers
// (identified through register 0x04) - copied from the Waveshare demo.
static const st77916_lcd_init_cmd_t vendor_specific_init_new[] = {
  {0xF0, (uint8_t[]){0x28}, 1, 0}, {0xF2, (uint8_t[]){0x28}, 1, 0}, {0x73, (uint8_t[]){0xF0}, 1, 0},
  {0x7C, (uint8_t[]){0xD1}, 1, 0}, {0x83, (uint8_t[]){0xE0}, 1, 0}, {0x84, (uint8_t[]){0x61}, 1, 0},
  {0xF2, (uint8_t[]){0x82}, 1, 0}, {0xF0, (uint8_t[]){0x00}, 1, 0}, {0xF0, (uint8_t[]){0x01}, 1, 0},
  {0xF1, (uint8_t[]){0x01}, 1, 0}, {0xB0, (uint8_t[]){0x56}, 1, 0}, {0xB1, (uint8_t[]){0x4D}, 1, 0},
  {0xB2, (uint8_t[]){0x24}, 1, 0}, {0xB4, (uint8_t[]){0x87}, 1, 0}, {0xB5, (uint8_t[]){0x44}, 1, 0},
  {0xB6, (uint8_t[]){0x8B}, 1, 0}, {0xB7, (uint8_t[]){0x40}, 1, 0}, {0xB8, (uint8_t[]){0x86}, 1, 0},
  {0xBA, (uint8_t[]){0x00}, 1, 0}, {0xBB, (uint8_t[]){0x08}, 1, 0}, {0xBC, (uint8_t[]){0x08}, 1, 0},
  {0xBD, (uint8_t[]){0x00}, 1, 0}, {0xC0, (uint8_t[]){0x80}, 1, 0}, {0xC1, (uint8_t[]){0x10}, 1, 0},
  {0xC2, (uint8_t[]){0x37}, 1, 0}, {0xC3, (uint8_t[]){0x80}, 1, 0}, {0xC4, (uint8_t[]){0x10}, 1, 0},
  {0xC5, (uint8_t[]){0x37}, 1, 0}, {0xC6, (uint8_t[]){0xA9}, 1, 0}, {0xC7, (uint8_t[]){0x41}, 1, 0},
  {0xC8, (uint8_t[]){0x01}, 1, 0}, {0xC9, (uint8_t[]){0xA9}, 1, 0}, {0xCA, (uint8_t[]){0x41}, 1, 0},
  {0xCB, (uint8_t[]){0x01}, 1, 0}, {0xD0, (uint8_t[]){0x91}, 1, 0}, {0xD1, (uint8_t[]){0x68}, 1, 0},
  {0xD2, (uint8_t[]){0x68}, 1, 0}, {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0}, {0xDD, (uint8_t[]){0x4F}, 1, 0},
  {0xDE, (uint8_t[]){0x4F}, 1, 0}, {0xF1, (uint8_t[]){0x10}, 1, 0}, {0xF0, (uint8_t[]){0x00}, 1, 0},
  {0xF0, (uint8_t[]){0x02}, 1, 0},
  {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
  {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
  {0xF0, (uint8_t[]){0x10}, 1, 0}, {0xF3, (uint8_t[]){0x10}, 1, 0}, {0xE0, (uint8_t[]){0x07}, 1, 0},
  {0xE1, (uint8_t[]){0x00}, 1, 0}, {0xE2, (uint8_t[]){0x00}, 1, 0}, {0xE3, (uint8_t[]){0x00}, 1, 0},
  {0xE4, (uint8_t[]){0xE0}, 1, 0}, {0xE5, (uint8_t[]){0x06}, 1, 0}, {0xE6, (uint8_t[]){0x21}, 1, 0},
  {0xE7, (uint8_t[]){0x01}, 1, 0}, {0xE8, (uint8_t[]){0x05}, 1, 0}, {0xE9, (uint8_t[]){0x02}, 1, 0},
  {0xEA, (uint8_t[]){0xDA}, 1, 0}, {0xEB, (uint8_t[]){0x00}, 1, 0}, {0xEC, (uint8_t[]){0x00}, 1, 0},
  {0xED, (uint8_t[]){0x0F}, 1, 0}, {0xEE, (uint8_t[]){0x00}, 1, 0}, {0xEF, (uint8_t[]){0x00}, 1, 0},
  {0xF8, (uint8_t[]){0x00}, 1, 0}, {0xF9, (uint8_t[]){0x00}, 1, 0}, {0xFA, (uint8_t[]){0x00}, 1, 0},
  {0xFB, (uint8_t[]){0x00}, 1, 0}, {0xFC, (uint8_t[]){0x00}, 1, 0}, {0xFD, (uint8_t[]){0x00}, 1, 0},
  {0xFE, (uint8_t[]){0x00}, 1, 0}, {0xFF, (uint8_t[]){0x00}, 1, 0}, {0x60, (uint8_t[]){0x40}, 1, 0},
  {0x61, (uint8_t[]){0x04}, 1, 0}, {0x62, (uint8_t[]){0x00}, 1, 0}, {0x63, (uint8_t[]){0x42}, 1, 0},
  {0x64, (uint8_t[]){0xD9}, 1, 0}, {0x65, (uint8_t[]){0x00}, 1, 0}, {0x66, (uint8_t[]){0x00}, 1, 0},
  {0x67, (uint8_t[]){0x00}, 1, 0}, {0x68, (uint8_t[]){0x00}, 1, 0}, {0x69, (uint8_t[]){0x00}, 1, 0},
  {0x6A, (uint8_t[]){0x00}, 1, 0}, {0x6B, (uint8_t[]){0x00}, 1, 0}, {0x70, (uint8_t[]){0x40}, 1, 0},
  {0x71, (uint8_t[]){0x03}, 1, 0}, {0x72, (uint8_t[]){0x00}, 1, 0}, {0x73, (uint8_t[]){0x42}, 1, 0},
  {0x74, (uint8_t[]){0xD8}, 1, 0}, {0x75, (uint8_t[]){0x00}, 1, 0}, {0x76, (uint8_t[]){0x00}, 1, 0},
  {0x77, (uint8_t[]){0x00}, 1, 0}, {0x78, (uint8_t[]){0x00}, 1, 0}, {0x79, (uint8_t[]){0x00}, 1, 0},
  {0x7A, (uint8_t[]){0x00}, 1, 0}, {0x7B, (uint8_t[]){0x00}, 1, 0}, {0x80, (uint8_t[]){0x48}, 1, 0},
  {0x81, (uint8_t[]){0x00}, 1, 0}, {0x82, (uint8_t[]){0x06}, 1, 0}, {0x83, (uint8_t[]){0x02}, 1, 0},
  {0x84, (uint8_t[]){0xD6}, 1, 0}, {0x85, (uint8_t[]){0x04}, 1, 0}, {0x86, (uint8_t[]){0x00}, 1, 0},
  {0x87, (uint8_t[]){0x00}, 1, 0}, {0x88, (uint8_t[]){0x48}, 1, 0}, {0x89, (uint8_t[]){0x00}, 1, 0},
  {0x8A, (uint8_t[]){0x08}, 1, 0}, {0x8B, (uint8_t[]){0x02}, 1, 0}, {0x8C, (uint8_t[]){0xD8}, 1, 0},
  {0x8D, (uint8_t[]){0x04}, 1, 0}, {0x8E, (uint8_t[]){0x00}, 1, 0}, {0x8F, (uint8_t[]){0x00}, 1, 0},
  {0x90, (uint8_t[]){0x48}, 1, 0}, {0x91, (uint8_t[]){0x00}, 1, 0}, {0x92, (uint8_t[]){0x0A}, 1, 0},
  {0x93, (uint8_t[]){0x02}, 1, 0}, {0x94, (uint8_t[]){0xDA}, 1, 0}, {0x95, (uint8_t[]){0x04}, 1, 0},
  {0x96, (uint8_t[]){0x00}, 1, 0}, {0x97, (uint8_t[]){0x00}, 1, 0}, {0x98, (uint8_t[]){0x48}, 1, 0},
  {0x99, (uint8_t[]){0x00}, 1, 0}, {0x9A, (uint8_t[]){0x0C}, 1, 0}, {0x9B, (uint8_t[]){0x02}, 1, 0},
  {0x9C, (uint8_t[]){0xDC}, 1, 0}, {0x9D, (uint8_t[]){0x04}, 1, 0}, {0x9E, (uint8_t[]){0x00}, 1, 0},
  {0x9F, (uint8_t[]){0x00}, 1, 0}, {0xA0, (uint8_t[]){0x48}, 1, 0}, {0xA1, (uint8_t[]){0x00}, 1, 0},
  {0xA2, (uint8_t[]){0x05}, 1, 0}, {0xA3, (uint8_t[]){0x02}, 1, 0}, {0xA4, (uint8_t[]){0xD5}, 1, 0},
  {0xA5, (uint8_t[]){0x04}, 1, 0}, {0xA6, (uint8_t[]){0x00}, 1, 0}, {0xA7, (uint8_t[]){0x00}, 1, 0},
  {0xA8, (uint8_t[]){0x48}, 1, 0}, {0xA9, (uint8_t[]){0x00}, 1, 0}, {0xAA, (uint8_t[]){0x07}, 1, 0},
  {0xAB, (uint8_t[]){0x02}, 1, 0}, {0xAC, (uint8_t[]){0xD7}, 1, 0}, {0xAD, (uint8_t[]){0x04}, 1, 0},
  {0xAE, (uint8_t[]){0x00}, 1, 0}, {0xAF, (uint8_t[]){0x00}, 1, 0}, {0xB0, (uint8_t[]){0x48}, 1, 0},
  {0xB1, (uint8_t[]){0x00}, 1, 0}, {0xB2, (uint8_t[]){0x09}, 1, 0}, {0xB3, (uint8_t[]){0x02}, 1, 0},
  {0xB4, (uint8_t[]){0xD9}, 1, 0}, {0xB5, (uint8_t[]){0x04}, 1, 0}, {0xB6, (uint8_t[]){0x00}, 1, 0},
  {0xB7, (uint8_t[]){0x00}, 1, 0}, {0xB8, (uint8_t[]){0x48}, 1, 0}, {0xB9, (uint8_t[]){0x00}, 1, 0},
  {0xBA, (uint8_t[]){0x0B}, 1, 0}, {0xBB, (uint8_t[]){0x02}, 1, 0}, {0xBC, (uint8_t[]){0xDB}, 1, 0},
  {0xBD, (uint8_t[]){0x04}, 1, 0}, {0xBE, (uint8_t[]){0x00}, 1, 0}, {0xBF, (uint8_t[]){0x00}, 1, 0},
  {0xC0, (uint8_t[]){0x10}, 1, 0}, {0xC1, (uint8_t[]){0x47}, 1, 0}, {0xC2, (uint8_t[]){0x56}, 1, 0},
  {0xC3, (uint8_t[]){0x65}, 1, 0}, {0xC4, (uint8_t[]){0x74}, 1, 0}, {0xC5, (uint8_t[]){0x88}, 1, 0},
  {0xC6, (uint8_t[]){0x99}, 1, 0}, {0xC7, (uint8_t[]){0x01}, 1, 0}, {0xC8, (uint8_t[]){0xBB}, 1, 0},
  {0xC9, (uint8_t[]){0xAA}, 1, 0}, {0xD0, (uint8_t[]){0x10}, 1, 0}, {0xD1, (uint8_t[]){0x47}, 1, 0},
  {0xD2, (uint8_t[]){0x56}, 1, 0}, {0xD3, (uint8_t[]){0x65}, 1, 0}, {0xD4, (uint8_t[]){0x74}, 1, 0},
  {0xD5, (uint8_t[]){0x88}, 1, 0}, {0xD6, (uint8_t[]){0x99}, 1, 0}, {0xD7, (uint8_t[]){0x01}, 1, 0},
  {0xD8, (uint8_t[]){0xBB}, 1, 0}, {0xD9, (uint8_t[]){0xAA}, 1, 0}, {0xF3, (uint8_t[]){0x01}, 1, 0},
  {0xF0, (uint8_t[]){0x00}, 1, 0}, {0x21, (uint8_t[]){0x00}, 1, 0}, {0x11, (uint8_t[]){0x00}, 1, 120},
  {0x29, (uint8_t[]){0x00}, 1, 0},
};

static esp_lcd_panel_handle_t s_panel = NULL;

static void lcd_hw_reset() {
  exio_set(EXIO_LCD_RST, false);
  vTaskDelay(pdMS_TO_TICKS(10));
  exio_set(EXIO_LCD_RST, true);
  vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_lcd_panel_io_handle_t new_io(int hz, esp_lcd_panel_io_color_trans_done_cb_t cb, void *ctx) {
  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = PIN_LCD_CS;
  io_config.dc_gpio_num = -1;
  io_config.spi_mode = 0;
  io_config.pclk_hz = hz;
  io_config.trans_queue_depth = 10;
  io_config.on_color_trans_done = cb;
  io_config.user_ctx = ctx;
  io_config.lcd_cmd_bits = 32;
  io_config.lcd_param_bits = 8;
  io_config.flags.quad_mode = 1;
  esp_lcd_panel_io_handle_t io = NULL;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io) != ESP_OK) return NULL;
  return io;
}

bool display_init(esp_lcd_panel_io_color_trans_done_cb_t doneCb, void *userCtx) {
  lcd_hw_reset();
  pinMode(PIN_LCD_TE, OUTPUT);

  spi_bus_config_t bus = {};
  bus.data0_io_num = PIN_LCD_D0;
  bus.data1_io_num = PIN_LCD_D1;
  bus.sclk_io_num = PIN_LCD_SCK;
  bus.data2_io_num = PIN_LCD_D2;
  bus.data3_io_num = PIN_LCD_D3;
  bus.data4_io_num = -1; bus.data5_io_num = -1; bus.data6_io_num = -1; bus.data7_io_num = -1;
  bus.max_transfer_sz = LCD_W * 48 * 2 + 16;
  bus.flags = SPICOMMON_BUSFLAG_MASTER;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    Serial.println("[LCD] spi_bus_initialize failed");
    return false;
  }

  // Probe register 0x04 at low speed to pick the right init sequence.
  uint8_t id[4] = {0};
  esp_lcd_panel_io_handle_t io = new_io(5 * 1000 * 1000, NULL, NULL);
  if (!io) { Serial.println("[LCD] panel io failed"); return false; }
  int cmd = (0x04 << 8) | (int)(LCD_OPCODE_READ_CMD << 24);
  if (esp_lcd_panel_io_rx_param(io, cmd, id, sizeof(id)) == ESP_OK) {
    Serial.printf("[LCD] reg04 = %02x %02x %02x %02x\n", id[0], id[1], id[2], id[3]);
  }
  esp_lcd_panel_io_del(io);

  io = new_io(LCD_QSPI_HZ, doneCb, userCtx);
  if (!io) { Serial.println("[LCD] panel io (fast) failed"); return false; }

  st77916_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  if (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
    vendor.init_cmds = vendor_specific_init_new;
    vendor.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(vendor_specific_init_new[0]);
    Serial.println("[LCD] using alternative init sequence");
  }

  esp_lcd_panel_dev_config_t pc = {};
  pc.reset_gpio_num = -1;
  pc.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  pc.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
  pc.bits_per_pixel = 16;
  pc.vendor_config = &vendor;
  if (esp_lcd_new_panel_st77916(io, &pc, &s_panel) != ESP_OK) {
    Serial.println("[LCD] new panel failed");
    return false;
  }
  esp_lcd_panel_reset(s_panel);
  esp_lcd_panel_init(s_panel);
  esp_lcd_panel_disp_on_off(s_panel, true);
  return true;
}

void display_draw(int x1, int y1, int x2, int y2, const void *rgb565) {
  if (!s_panel) return;
  if (x2 > LCD_W) x2 = LCD_W;
  if (y2 > LCD_H) y2 = LCD_H;
  esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, rgb565);
}

void display_on(bool on) {
  if (s_panel) esp_lcd_panel_disp_on_off(s_panel, on);
}
