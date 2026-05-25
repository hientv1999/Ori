#pragma once

#include <lvgl.h>

// Ori — Unpair phone confirmation modal.
//
// Triggered when the user wants to remove the current phone BLE bond.
// Centered alert card with phone icon, "Unpair phone?" heading, body copy,
// and Cancel + Unpair (danger) buttons.
// M3 wires Cancel to dismiss; Unpair is a stub (M4 wires to actual bond wipe).

namespace modal_unpair_phone {

lv_obj_t* create(lv_obj_t* base_screen);

} // namespace modal_unpair_phone
