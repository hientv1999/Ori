#pragma once

#include <lvgl.h>

// Ori — Unpair phone confirmation modal.
//
// Triggered when the user wants to remove the current phone BLE bond.
// Centered alert card sharing modal_factory_reset's generic warning-circle
// glyph (M8: swap in a proper phone-shaped asset), a dynamic "Unpair
// iPhone?"/"Unpair iPad?"/"Unpair iPhone or iPad?" heading (picked from
// ancs_client::phone_kind_word() — see the .cpp), body copy, and
// Cancel + Unpair (danger) buttons.
// Cancel dismisses; Unpair calls state_machine::on_unpair_phone(), which
// wipes the bond. The user re-pairs later via the status-bar phone icon.

namespace modal_unpair_phone {

lv_obj_t* create(lv_obj_t* base_screen);

} // namespace modal_unpair_phone
