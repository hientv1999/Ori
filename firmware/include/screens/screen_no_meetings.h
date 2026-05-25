#pragma once

#include <lvgl.h>

// Ori — "No meetings today" empty-state screen.
// Used when work hours but the cached list has zero items.

namespace screen_no_meetings {

lv_obj_t* create();

} // namespace screen_no_meetings
