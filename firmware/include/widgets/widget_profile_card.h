#pragma once

#include <lvgl.h>

// Ori — right panel profile card. 269 px wide, fills below the status bar.
//
// Visual: circular 228 x 228 profile photo (decoded JPEG from photo_cache),
// full name, job title. Mirrors the prototype's .right-panel + .profile-photo block.

namespace widget_profile_card {

constexpr int16_t WIDTH       = 269;
constexpr int16_t PHOTO_SIZE  = 228;

// Ori's own BLE connection status to Orion (Device Status, `ble-protocol.md`
// §3) — derived entirely on-device from state_machine.cpp/ble_manager.cpp's
// existing connection tracking, never pushed over the wire.
enum class ConnStatus : uint8_t {
    Disconnected = 0,
    Syncing      = 1,  // RUNTIME_RECONNECTING / RUNTIME_SYNCING (and the
                        // equivalent setup-time syncing states)
    Connected    = 2,  // RUNTIME_READY
};

// Weather condition enum matching the BLE Device Settings "w" field
// (`ble-protocol.md` §3/§4, char 000E). Drives the weather-icon glyph
// overlaid on the profile photo (`screen-layout.md` "Weather icon +
// temperature text").
enum class WeatherCondition : uint8_t {
    Clear        = 0,
    PartlyCloudy = 1,
    Cloudy       = 2,
    Rain         = 3,
    Thunderstorm = 4,
    Snow         = 5,
    Fog          = 6,
};

// Precipitation intensity matching the BLE Device Settings "i" field
// (`ble-protocol.md` §3/§4, char 000E). Only Rain/Thunderstorm/Snow use all
// three non-None levels; Fog only ever arrives as Light or Heavy (its icon
// has no separate "moderate" glyph). None for Clear/PartlyCloudy/Cloudy,
// which have no intensity axis at all.
enum class WeatherIntensity : uint8_t {
    None     = 0,
    Light    = 1,
    Moderate = 2,
    Heavy    = 3,
};

// Temperature unit enum matching the BLE Device Settings "u" field
// (`ble-protocol.md` §3/§4, char 000E). Orion declares the unit; Ori never
// converts, it just renders the integer + this unit's letter ("72°F"/"22°C").
enum class TemperatureUnit : uint8_t {
    Fahrenheit = 0,
    Celsius    = 1,
};

lv_obj_t* create(lv_obj_t* parent);

// Update the profile-photo border color to reflect Ori's current connection
// status to Orion. Cheap call — sets one style property; safe to call
// from state_machine.cpp/ble_manager.cpp on every Device Status transition.
void set_conn_status(lv_obj_t* card, ConnStatus s);

// Default connection status applied to newly-created cards. Set before
// calling create() to influence the initial border colour. Defaults to
// Disconnected so the device never flashes a stale "Connected" green before
// the BLE link to Orion is actually established.
void set_default_conn_status(ConnStatus s);

// Returns the current default connection status (the value passed to the
// last set_default_conn_status() call, or Disconnected before any call).
// Used by modal_profile to match the profile-card border colour.
ConnStatus get_default_conn_status();

// ConnStatus -> theme::COLOR_CONN_* border/glow colour. Shared table so the
// profile card, modal_profile's own photo ring, and anything else that
// renders connection status can't drift out of sync with each other.
uint32_t conn_status_color(ConnStatus s);

// Register a photo object (lv_obj_t circle) whose border colour should track
// connection-status changes in real time. Intended for modal_profile — call
// on open, unregister on the scrim's LV_EVENT_DELETE. Only one observer at a time.
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
// set_profile() can push updates to the overlay without it being closed and
// re-opened. Unregister on scrim LV_EVENT_DELETE.
struct ModalLabels {
    lv_obj_t* name_lbl;
    lv_obj_t* title_lbl;
    lv_obj_t* email_lbl;   // nullptr when email was empty at open time
    lv_obj_t* phone_lbl;   // nullptr when phone was empty at open time
};
void register_modal_labels(const ModalLabels& labels);
void unregister_modal_labels();

// Sets the profile photo on the currently active profile card.
// Pass a decoded RGB565 lv_image_dsc_t (from photo_cache::get()) to show the
// user photo; pass nullptr to hide the image (dark circle bg is shown).
// Also stores the descriptor pointer as the new default so cards created after
// this call (screen transitions) start with the correct photo.
void set_photo(const lv_image_dsc_t* img_dsc);

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

// Update the weather icon + temperature text on the given card.
// visible=false hides BOTH elements entirely (LV_OBJ_FLAG_HIDDEN, no placeholder
// glyph) — used before the first weather data ever arrives, and whenever the
// BLE-PC link is down (ble-protocol.md §6.4 — "don't show what can't be
// verified"). When visible=true, condition/temp_f/unit/is_night/intensity are
// applied and both elements are shown. is_night swaps the sun for a moon
// (every condition also gets a few background stars); intensity is ignored
// for Clear/PartlyCloudy/Cloudy.
void set_weather(lv_obj_t* card, WeatherCondition condition, int temp_f,
                  TemperatureUnit unit, bool visible, bool is_night,
                  WeatherIntensity intensity);

// Default weather applied to newly-created cards — mirrors set_default_conn_status().
// Call this (not set_weather directly) from application code; it updates
// g_active_card itself, same pattern as set_default_conn_status().
void set_default_weather(WeatherCondition condition, int temp_f,
                          TemperatureUnit unit, bool visible, bool is_night,
                          WeatherIntensity intensity);

// Builds a standalone weather-condition glyph — the SAME composition
// helpers the profile card's own badge uses internally (build_weather_icon()
// in widget_profile_card.cpp), just not wired to the card's corner position.
// For contexts outside the profile card that want the identical icon — e.g.
// modal_device_alert.cpp's Weather Alert overlay. Creates a fresh
// BADGE_SIZE (60x60) container, centered in `parent` (lv_obj_center()) —
// size `parent` generously (a ~64-76px circle works well) so the glyph isn't
// clipped. Returns the built container.
lv_obj_t* create_weather_glyph(lv_obj_t* parent, WeatherCondition condition,
                                bool is_night, WeatherIntensity intensity);

} // namespace widget_profile_card
