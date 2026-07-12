#pragma once

#include <lvgl.h>

// Ori — Unpair phone confirmation modal.
//
// Triggered when the user wants to remove the current phone BLE bond.
// Centered alert card with phone icon, "Unpair phone?" heading, body copy,
// and Cancel + Unpair (danger) buttons.
// Cancel dismisses; Unpair calls state_machine::on_unpair_phone(), which
// wipes the bond. The user re-pairs later via the status-bar phone icon.

namespace modal_unpair_phone {

lv_obj_t* create(lv_obj_t* base_screen);

} // namespace modal_unpair_phone
