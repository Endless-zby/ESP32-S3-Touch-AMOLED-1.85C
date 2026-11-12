#include "lvgl_port.h"
#include "config.h"
#include "display.h"
#include "touch.h"
#include <esp_heap_caps.h>

#define BUF_LINES 24

static lv_display_t *s_disp = NULL;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx) {
  lv_display_flush_ready((lv_display_t *)ctx);
  return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  lv_draw_sw_rgb565_swap(px_map, w * h);
  display_draw(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
  // flush_ready is signalled from on_color_done()
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *data) {
  uint16_t x, y;
  if (touch_read(x, y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static uint32_t tick_cb() { return millis(); }

bool lvgl_port_init() {
  lv_init();
  lv_tick_set_cb(tick_cb);

  s_disp = lv_display_create(LCD_W, LCD_H);
  if (!display_init(on_color_done, s_disp)) return false;

  size_t bufBytes = LCD_W * BUF_LINES * 2;
  void *b1 = heap_caps_malloc(bufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void *b2 = heap_caps_malloc(bufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!b1 || !b2) {
    Serial.println("[LVGL] draw buffer alloc failed");
    return false;
  }
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(s_disp, flush_cb);
  lv_display_set_buffers(s_disp, b1, b2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_cb);
  return true;
}

void lvgl_port_loop() {
  lv_timer_handler();
}
