#pragma once

#include <lvgl.h>

// Ori — digital clock screen (Clock view), entered by tapping the
// status-bar time from any runtime state.
//
// Layout: status bar (date/time hidden) + huge digital clock in the left
// panel + profile card on the right. Colon breathes at 0.5 Hz (fades
// between full and 25% opacity) rather than a hard on/off blink.

namespace screen_clock {

lv_obj_t* create();

} // namespace screen_clock
