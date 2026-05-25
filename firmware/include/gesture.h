#pragma once
#include <stdint.h>
#include "touch_gt911.h"

// Two-finger backlight swipe gesture.
//
// Per gestures.md / memory.md:
//   - exactly two pressed points
//   - engagement requires >= 80 ms presence AND >= 60 px vertical movement
//     from the initial two-finger touchdown
//   - one-shot per gesture: fires once on the engagement boundary, then
//     stays inert until both fingers lift
//   - direction:  net upward swipe (dy < 0) -> backlight ON
//                 net downward swipe (dy > 0) -> backlight OFF
//   - idempotent: target-state matches current state -> no-op (no extra
//     BLE notify / NVS write)
//
// This module does NOT gate single-touch into LVGL — esp32-lvgl sees the
// raw touch count and decides whether to forward to the UI.
namespace gesture {

void update(const TouchPoint points[5], uint8_t count);

} // namespace gesture
