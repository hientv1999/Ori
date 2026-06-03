#pragma once

#include <lvgl.h>

// Ori — first-time setup flow.
//
// Single screen, five visual sub-states. Status bar is hidden across the
// entire flow. A 4-dot progress indicator is anchored at a FIXED y near
// the bottom — it must not move between pages, per setup-flow.md.
//
//   Welcome       — dots all inactive, brand + Start
//   Install       — dot 0 active, "Install Orion" copy
//   Pairing       — dot 1 active, "Link Orion" heading + BLE name + spinner
//   PhonePairing  — dot 2 active, phone-pairing copy + Skip
//   Complete      — dots HIDDEN, brief acknowledgement
//
// Two modal overlays sit on top of the Pairing sub-state:
//   Passkey modal   — show_passkey_modal / hide_passkey_modal
//   Orioning modal  — show_orioning_modal / hide_orioning_modal

namespace screen_setup {

enum class Step {
    Welcome,
    Install,
    Pairing,
    PhonePairing,
    Complete,
};

lv_obj_t* create(Step initial);
void      set_step(lv_obj_t* screen, Step s);

// Pairing-step modal overlays. Both sit on top of the Pairing base screen.
// show_* returns the modal object; callers may ignore it.
lv_obj_t* show_passkey_modal(lv_obj_t* screen);
void      hide_passkey_modal(lv_obj_t* screen);

lv_obj_t* show_orioning_modal(lv_obj_t* screen);
void      hide_orioning_modal(lv_obj_t* screen);

} // namespace screen_setup
