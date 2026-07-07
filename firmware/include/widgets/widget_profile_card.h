#pragma once

#include <lvgl.h>

// Ori — right panel profile card. 269 px wide, fills below the status bar.
//
// Visual: circular 228 x 228 profile photo (decoded JPEG from photo_cache),
// full name, job title. Mirrors the prototype's .right-panel + .profile-photo block.

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

// Weather condition enum matching the BLE Device Settings "w" field
// (`ble-protocol.md` §3/§4, char 000E). Drives the weather-badge glyph
// overlaid on the profile photo (`screen-layout.md` "Weather badge +
// temperature bubble").
enum class WeatherCondition : uint8_t {
    Clear        = 0,
    PartlyCloudy = 1,
    Cloudy       = 2,
    Rain         = 3,
    Thunderstorm = 4,
    Snow         = 5,
    Fog          = 6,
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

// Register a photo object (lv_obj_t circle) whose border colour should track
// presence changes in real time. Intended for modal_profile — call on open,
// unregister on the scrim's LV_EVENT_DELETE. Only one observer at a time.
void register_modal_photo(lv_obj_t* photo_obj);
void unregister_modal_photo();

// Register the modal_profile photo image (lv_image) so set_photo() updates it
// live while the detail overlay is open — otherwise a photo that arrives over
// BLE while the modal is up wouldn't appear until it's closed and reopened.
// Call on open, unregister on the screen's LV_EVENT_DELETE. One observer.
void register_modal_photo_img(lv_obj_t* img_obj);
void unregister_modal_photo_img();

// Live-update handles for the modal_profile text labels. All fields may be
// nullptr (email/phone labels are only created when data is non-empty).
// register_modal_labels() is called after create() builds all labels so that
// set_default_presence() and set_profile() can push updates to the overlay
// without it being closed and re-opened. Unregister on scrim LV_EVENT_DELETE.
struct ModalLabels {
    lv_obj_t* status_lbl;  // presence word ("Available"…) — updated by set_default_presence()
    lv_obj_t* name_lbl;
    lv_obj_t* title_lbl;
    lv_obj_t* email_lbl;   // nullptr when email was empty at open time
    lv_obj_t* phone_lbl;   // nullptr when phone was empty at open time
    lv_obj_t* status_dot;  // presence-coloured dot beside status_lbl; nullptr if absent
};
void register_modal_labels(const ModalLabels& labels);
void unregister_modal_labels();

// Sets the profile photo on the currently active profile card.
// Pass a decoded RGB565 lv_image_dsc_t (from photo_cache::get()) to show the
// user photo; pass nullptr to hide the image (dark circle bg is shown).
// Also stores the descriptor pointer as the new default so cards created after
// this call (screen transitions) start with the correct photo.
void set_photo(const lv_image_dsc_t* img_dsc);

// Returns the cached photo descriptor set by the last set_photo() call, or
// nullptr if no photo has been provided.  Called by create() so new screens
// start with the photo already loaded without an explicit post-create call.
const lv_image_dsc_t* get_photo();

// Store the profile fields that create() and modal_profile use for newly built
// cards, and update the live card's labels immediately (if one exists on screen).
// Call at boot (from NVS) and on every BLE ProfileInfo write.
// email and phone may be nullptr to leave those fields unchanged.
void set_profile(const char* name, const char* title,
                 const char* email = nullptr, const char* phone = nullptr);

// Read back cached profile fields (populated by set_profile()).
// Returns empty string if set_profile() has not been called yet.
const char* get_profile_name();
const char* get_profile_title();
const char* get_profile_email();
const char* get_profile_phone();

// Update the weather badge + temperature bubble on the given card.
// visible=false hides BOTH elements entirely (LV_OBJ_FLAG_HIDDEN, no placeholder
// glyph) — used before the first weather data ever arrives, and whenever the
// BLE-PC link is down (ble-protocol.md §6.4 — same "don't show what can't be
// verified" policy as presence-offline). When visible=true, condition/temp_f
// are applied and both elements are shown.
void set_weather(lv_obj_t* card, WeatherCondition condition, int temp_f, bool visible);

// Default weather applied to newly-created cards — mirrors set_default_presence().
// Call this (not set_weather directly) from application code; it updates
// g_active_card itself, same pattern as set_default_presence().
void set_default_weather(WeatherCondition condition, int temp_f, bool visible);

} // namespace widget_profile_card
