#pragma once

#include <lvgl.h>

// Ori — "No meetings today" empty-state screen.
// Used when work hours but the cached list has zero items.

namespace screen_no_meetings {

lv_obj_t* create();

// Builds JUST the left panel (the calendar glyph + "No meetings today" copy)
// as a child of `body`, returning it. Split out from create() so the state
// machine can rebuild only the left panel in place on a reconnect sync,
// without tearing down the shared status bar + profile card
// (state_machine.cpp refresh_runtime_left). create() itself calls this.
lv_obj_t* build_left(lv_obj_t* body);

} // namespace screen_no_meetings
