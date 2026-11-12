#pragma once
#include <lvgl.h>

bool lvgl_port_init();
void lvgl_port_loop();   // call from the thread that owns LVGL (Arduino loop)
