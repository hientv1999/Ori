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

// prev_screen: the runtime screen to return to when Skip is tapped on the
// PhonePairing step.  Pass nullptr during the initial setup flow (Skip → Complete).
// Pass the live runtime screen during the runtime re-pair-iPhone flow (Skip → back).
// The caller must load the new screen with auto_del=false so prev_screen stays alive.
lv_obj_t* create(Step initial, lv_obj_t* prev_screen = nullptr);
void      set_step(lv_obj_t* screen, Step s);

// Pairing-step modal overlays. Both sit on top of the Pairing base screen.
// show_* returns the modal object; callers may ignore it.
// passkey must be 0..999999; it is formatted as a zero-padded 6-digit string.
lv_obj_t* show_passkey_modal(lv_obj_t* screen, uint32_t passkey);
void      hide_passkey_modal(lv_obj_t* screen);

lv_obj_t* show_orioning_modal(lv_obj_t* screen);
void      hide_orioning_modal(lv_obj_t* screen);

// Update the progress ring inside the orioning modal (0–100).
// No-op if the modal is not currently visible.
void      update_orioning_progress(lv_obj_t* screen, uint8_t pct);

} // namespace screen_setup
