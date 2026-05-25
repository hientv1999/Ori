#pragma once

#include <lvgl.h>

// Ori — runtime re-pair phone screen.
//
// Same visual as setup Step 4 BUT with Cancel instead of Skip. Status bar
// is hidden so the layout is pixel-identical to Step 4 — setup-flow.md
// requires the spinner have "the same room" as Step 4.
//
// Trigger (M4): long-press the phone-disconnect icon for 3 s from any
// runtime state.

namespace screen_repair_phone {

lv_obj_t* create();

} // namespace screen_repair_phone
