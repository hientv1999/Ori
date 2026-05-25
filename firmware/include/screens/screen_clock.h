#pragma once

#include <lvgl.h>

// Ori — after-hours digital clock screen.
//
// Layout: status bar (date/time hidden) + huge digital clock in the left
// panel + profile card on the right. Colon blinks at 1 s intervals.

namespace screen_clock {

lv_obj_t* create();

} // namespace screen_clock
