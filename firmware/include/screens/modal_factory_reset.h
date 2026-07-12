#pragma once

#include <lvgl.h>

// Ori — Factory reset confirmation modal.
//
// Triggered by long-pressing the profile photo for 3 s. Centered alert card
// with warning icon, "Reset Ori?" heading, body copy, and Cancel + Reset
// buttons. Cancel dismisses; Reset calls factory_reset::execute() (deferred
// one tick — see the .cpp).

namespace modal_factory_reset {

lv_obj_t* create(lv_obj_t* base_screen);

} // namespace modal_factory_reset
