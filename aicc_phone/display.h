#pragma once
#include <Arduino.h>
#include "esp_lcd_panel_io.h"

// Initialise the ST77916 QSPI panel. `doneCb` is called (ISR context) when a
// colour transfer queued by display_draw() has completed.
bool display_init(esp_lcd_panel_io_color_trans_done_cb_t doneCb, void *userCtx);
// Draw RGB565 (already byte swapped to panel order). x2/y2 are exclusive.
void display_draw(int x1, int y1, int x2, int y2, const void *rgb565);
void display_on(bool on);
