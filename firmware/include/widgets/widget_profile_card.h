#pragma once

#include <lvgl.h>

// Ori — right panel profile card. 269 px wide, fills below the status bar.
//
// Visual: circular 228 x 228 photo placeholder (initials text — no JPEG
// decoder in M3), full name, job title. Mirrors the prototype's
// .right-panel + .profile-photo block.

namespace widget_profile_card {

constexpr int16_t WIDTH       = 269;
constexpr int16_t PHOTO_SIZE  = 228;

// Teams-presence enum matching the BLE Presence Status characteristic
// (`ble-protocol.md` §3 char 16). The device-side fallback when the BLE
// PC link is down is to render `Offline` regardless of the last-cached
// value Orion pushed — the photo can't claim a presence we can't verify.
enum class Presence : uint8_t {
    Available = 0x00,
    Busy      = 0x01,
    Away      = 0x02,
    Offline   = 0x03,
};

lv_obj_t* create(lv_obj_t* parent);

// Long-press handling (factory reset) is wired in M4. M3 exposes the
// photo container so a future caller can attach event callbacks.
lv_obj_t* photo_object(lv_obj_t* card);

// Update the profile-photo border color to reflect the user's current
// Teams presence (or the offline fallback when the PC link is down).
// Cheap call — sets one style property; safe to call from the BLE
// receive handler on every Presence Status write.
void set_presence(lv_obj_t* card, Presence p);

// Default presence applied to newly-created cards. Set before calling
// create() to influence the initial border colour. Defaults to Offline
// so the device never flashes a stale "Available" green before the BLE
// Presence Status characteristic delivers a value.
void set_default_presence(Presence p);

// Returns the current default presence (the value passed to the last
// set_default_presence() call, or Offline before any call). Used by
// modal_profile to match the profile-card border colour.
Presence get_default_presence();

// Sets the profile photo on the currently active profile card.
// Pass a decoded RGB565 lv_image_dsc_t (from photo_cache::get()) to show the
// real photo; pass nullptr to revert to the Ori wordmark / initials placeholder.
// Also stores the descriptor pointer as the new default so cards created after
// this call (screen transitions) start with the correct photo.
void set_photo(const lv_image_dsc_t* img_dsc);

// Returns the cached photo descriptor set by the last set_photo() call, or
// nullptr if no photo has been provided.  Called by create() so new screens
// start with the photo already loaded without an explicit post-create call.
const lv_image_dsc_t* get_photo();

} // namespace widget_profile_card
