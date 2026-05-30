#pragma once
#include <stdint.h>

#include <lvgl.h>
#include "touch_gt911.h"

// LVGL pointer input device backed by the GT911 touch driver.
//
// The GT911 hardware only delivers one frame per INT pulse and touch::poll()
// clears the status register on read. To avoid double-polling (gesture +
// LVGL fighting over the same frame), main.cpp polls once per loop and
// hands the result to feed() — LVGL's read_cb reads from that cached state.
//
// Rules applied in read_cb:
//   - 0 touches            -> RELEASED
//   - 1 touch              -> PRESSED at point[0], clamped to panel rect
//   - >=2 touches          -> RELEASED (two-finger backlight swipe gesture
//                                       owns the surface; single-touch suspended)

namespace lvgl_input {

void init();                                    // registers LVGL input device
lv_indev_t* get();                              // returns the registered indev handle
void feed(const TouchPoint* points, uint8_t n); // called once per loop tick

} // namespace lvgl_input
