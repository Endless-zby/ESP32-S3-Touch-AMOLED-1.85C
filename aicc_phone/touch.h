#pragma once
#include <Arduino.h>

bool touch_init();
// Returns true while the panel is touched; fills x/y.
bool touch_read(uint16_t &x, uint16_t &y);
