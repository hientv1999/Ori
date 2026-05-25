#pragma once

#include <lvgl.h>

// Ori — Factory reset confirmation modal.
//
// Triggered (in M4) by long-pressing the profile photo for 3 s. Centered
// alert card with warning icon, "Reset Ori?" heading, body copy, and
// Cancel + Reset buttons. M3 wires Cancel to dismiss; Reset is a stub.

namespace modal_factory_reset {

lv_obj_t* create(lv_obj_t* base_screen);

} // namespace modal_factory_reset
