#pragma once

// Ori — visual theme tokens.
//
// Mirrors the CSS variables in Ori_UI_Prototype.html (:root block):
//   --screen-bg   #000000   COLOR_BG
//   --screen-card #0F1217   COLOR_CARD
//   --screen-elev #161B23   COLOR_ELEV
//   --divider     rgba(255,255,255,0.10)        COLOR_DIVIDER
//   --divider-strong rgba(255,255,255,0.14)     COLOR_DIVIDER_STRONG
//   --text-1      #ECEEF1   COLOR_TEXT_PRIMARY
//   --text-2      #9097A1   COLOR_TEXT_SECONDARY
//   --text-3      #565B65   COLOR_TEXT_TERTIARY
//   --accent      #E0B86A   COLOR_ACCENT
//   --accent-soft rgba(224,184,106,0.14)        COLOR_ACCENT_SOFT (approx)
//   --accent-line rgba(224,184,106,0.55)        COLOR_ACCENT_LINE (approx)
//   --danger      #D86A6A   COLOR_DANGER
//   --ok          #7FB48A   COLOR_OK
//
// LVGL doesn't model true alpha tokens, so the *_SOFT / *_LINE colors are
// captured as opaque RGB values flattened against COLOR_BG (#000000). Where
// actual transparency is needed (e.g. modal scrim) we apply lv_opa_t at the
// site of use.

#include <lvgl.h>

namespace theme {

constexpr uint32_t COLOR_BG               = 0x000000;
constexpr uint32_t COLOR_CARD             = 0x0F1217;
constexpr uint32_t COLOR_ELEV             = 0x161B23;
constexpr uint32_t COLOR_DIVIDER          = 0x1A1A1A;   // 10% white over black
constexpr uint32_t COLOR_DIVIDER_STRONG   = 0x242424;   // 14% white over black
constexpr uint32_t COLOR_TEXT_PRIMARY     = 0xECEEF1;
constexpr uint32_t COLOR_TEXT_SECONDARY   = 0x9097A1;
constexpr uint32_t COLOR_TEXT_TERTIARY    = 0x565B65;
constexpr uint32_t COLOR_ACCENT           = 0xE0B86A;
constexpr uint32_t COLOR_ACCENT_SOFT      = 0x1F1A0F;   // accent at 14% over black
constexpr uint32_t COLOR_ACCENT_LINE      = 0x7B653A;   // accent at 55% over black
constexpr uint32_t COLOR_DANGER           = 0xD86A6A;
constexpr uint32_t COLOR_DANGER_SOFT      = 0x1E0F0F;   // danger at 14% over black
constexpr uint32_t COLOR_OK               = 0x7FB48A;
constexpr uint32_t COLOR_OK_SOFT          = 0x121913;   // ok at 14% over black
constexpr uint32_t COLOR_SCRIM            = 0x000000;   // modal scrim base (pure black)
constexpr lv_opa_t  SCRIM_OPA             = LV_OPA_90;  // opacity for every overlay scrim

// Teams-presence palette for the profile-photo border. Colors match the
// actual Microsoft Teams presence swatches so users get instant recognition.
// Maps the BLE Presence Status characteristic enum to display colors:
//   0x00 AVAILABLE → Teams green  (COLOR_PRESENCE_AVAILABLE)
//   0x01 BUSY      → Teams red    (COLOR_PRESENCE_BUSY)
//   0x02 AWAY      → Teams amber  (COLOR_PRESENCE_AWAY)
//   0x03 OFFLINE   → Teams grey   (COLOR_PRESENCE_OFFLINE) — also the device-
//                                  side fallback when the PC link is down.
constexpr uint32_t COLOR_PRESENCE_AVAILABLE      = 0x92C353;  // Teams green
constexpr uint32_t COLOR_PRESENCE_BUSY           = 0xC4314B;  // Teams red
constexpr uint32_t COLOR_PRESENCE_AWAY           = 0xFFAA44;  // Teams amber
constexpr uint32_t COLOR_PRESENCE_OFFLINE        = 0x8A8884;  // Teams grey
// Presence ring gradient: very light (top) → deep dark (bottom).
// This maximises visible contrast across the full ring height.
// Standard COLOR_PRESENCE_* are kept for UI text / status labels only.
constexpr uint32_t COLOR_PRESENCE_AVAILABLE_LIGHT = 0xD4FFAA;  // near-white lime
constexpr uint32_t COLOR_PRESENCE_BUSY_LIGHT      = 0xFFBBC8;  // near-white rose
constexpr uint32_t COLOR_PRESENCE_AWAY_LIGHT      = 0xFFF2AA;  // near-white amber
constexpr uint32_t COLOR_PRESENCE_OFFLINE_LIGHT   = 0xE8E8E8;  // near-white silver
constexpr uint32_t COLOR_PRESENCE_AVAILABLE_DARK  = 0x2A5C08;  // deep forest green
constexpr uint32_t COLOR_PRESENCE_BUSY_DARK       = 0x6E0E20;  // deep crimson
constexpr uint32_t COLOR_PRESENCE_AWAY_DARK       = 0x8C4400;  // deep burnt amber
constexpr uint32_t COLOR_PRESENCE_OFFLINE_DARK    = 0x282826;  // near-black charcoal

// Font getters — return pointers to Hanken Grotesk Medium fonts.
// Sizes were selected to mirror the prototype's CSS scale.
const lv_font_t* font_body();         // 20 px — default text
const lv_font_t* font_meta();         // 24 px — meeting meta line, status bar date/sep, calendar day numbers
const lv_font_t* font_title();        // 26 px — meeting title, headings
const lv_font_t* font_h2();           // 28 px — setup section headings
const lv_font_t* font_time();         // 30 px — status bar time
const lv_font_t* font_display();      // 36 px — empty state headline / setup welcome
const lv_font_t* font_large();        // 48 px — passkey modal
const lv_font_t* font_clock();        // 48 px — base font for the clock; UI scales 2x

// Apply default page styling (background color, no padding, no border) to
// a top-level screen object.
void apply_to_screen(lv_obj_t* screen);

// Helper — converts the uint32_t hex tokens above to lv_color_t.
inline lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }

} // namespace theme
