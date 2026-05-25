#pragma once

#include <lvgl.h>

// Ori — first-time setup flow.
//
// Single screen, six visual sub-states. Status bar is hidden across the
// entire flow. A 4-dot progress indicator is anchored at a FIXED y near
// the bottom — it must not move between pages, per setup-flow.md.
//
//   Welcome       — dots all inactive, brand + Start
//   Install       — dot 0 active, "Install Orion" copy
//   Pairing       — dot 1 active, BLE name + spinner
//   Orioning      — dot 2 active, progress ring
//   PhonePairing  — dot 3 active, phone-pairing copy + Skip
//   Complete      — dots HIDDEN, brief acknowledgement

namespace screen_setup {

enum class Step {
    Welcome,
    Install,
    Pairing,
    Orioning,
    PhonePairing,
    Complete,
};

lv_obj_t* create(Step initial);
void      set_step(lv_obj_t* screen, Step s);

// The Step 2 passkey is a centered modal overlay on top of the Pairing
// sub-state. Returns the modal object so callers can dismiss it.
lv_obj_t* show_passkey_modal(lv_obj_t* screen);
void      hide_passkey_modal(lv_obj_t* screen);

} // namespace screen_setup
