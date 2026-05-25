#pragma once

// Ori — visual theme tokens.
//
// Mirrors the CSS variables in Ori_UI_Prototype.html (:root block):
//   --screen-bg   #0E1116   COLOR_BG
//   --screen-card #161A21   COLOR_CARD
//   --screen-elev #1B2029   COLOR_ELEV
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
// captured as opaque RGB values flattened against COLOR_BG. Where actual
// transparency is needed (e.g. modal scrim) we apply lv_opa_t at the
// site of use.

#include <lvgl.h>

namespace theme {

constexpr uint32_t COLOR_BG               = 0x0E1116;
constexpr uint32_t COLOR_CARD             = 0x161A21;
constexpr uint32_t COLOR_ELEV             = 0x1B2029;
constexpr uint32_t COLOR_DIVIDER          = 0x323539;   // ~15% white over bg
constexpr uint32_t COLOR_DIVIDER_STRONG   = 0x3E4145;   // ~20% white over bg
constexpr uint32_t COLOR_TEXT_PRIMARY     = 0xECEEF1;
constexpr uint32_t COLOR_TEXT_SECONDARY   = 0x9097A1;
constexpr uint32_t COLOR_TEXT_TERTIARY    = 0x565B65;
constexpr uint32_t COLOR_ACCENT           = 0xE0B86A;
constexpr uint32_t COLOR_ACCENT_SOFT      = 0x2A2519;   // accent at 14% over bg
constexpr uint32_t COLOR_ACCENT_LINE      = 0x8B7041;   // accent at 55% over bg
constexpr uint32_t COLOR_DANGER           = 0xD86A6A;
constexpr uint32_t COLOR_DANGER_SOFT      = 0x2A1518;   // danger at 14% over bg
constexpr uint32_t COLOR_OK               = 0x7FB48A;
constexpr uint32_t COLOR_OK_SOFT          = 0x182519;   // ok at 14% over bg
constexpr uint32_t COLOR_SCRIM            = 0x07080B;   // modal scrim base
constexpr lv_opa_t  SCRIM_OPA             = LV_OPA_90;  // opacity for every overlay scrim

// Teams-presence palette for the profile-photo border. Colors match the
// actual Microsoft Teams presence swatches so users get instant recognition.
// Maps the BLE Presence Status characteristic enum to display colors:
//   0x00 AVAILABLE → Teams green  (COLOR_PRESENCE_AVAILABLE)
//   0x01 BUSY      → Teams red    (COLOR_PRESENCE_BUSY)
//   0x02 AWAY      → Teams amber  (COLOR_PRESENCE_AWAY)
//   0x03 OFFLINE   → Teams grey   (COLOR_PRESENCE_OFFLINE) — also the device-
//                                  side fallback when the PC link is down.
constexpr uint32_t COLOR_PRESENCE_AVAILABLE = 0x92C353;  // Teams green
constexpr uint32_t COLOR_PRESENCE_BUSY      = 0xC4314B;  // Teams red
constexpr uint32_t COLOR_PRESENCE_AWAY      = 0xFFAA44;  // Teams amber
constexpr uint32_t COLOR_PRESENCE_OFFLINE   = 0x8A8884;  // Teams grey

// Font getters — return pointers to LVGL's bundled Montserrat fonts.
// Sizes were selected to mirror the prototype's CSS scale.
const lv_font_t* font_small();        // 16 px — pill, secondary meta
const lv_font_t* font_body();         // 20 px — default text
const lv_font_t* font_meta();         // 22 px — meeting meta line, status bar date/sep
const lv_font_t* font_title();        // 24 px — meeting title, headings
const lv_font_t* font_h2();           // 28 px — setup section headings
const lv_font_t* font_time();         // 30 px — status bar time
const lv_font_t* font_display();      // 36 px — empty state headline / setup welcome
const lv_font_t* font_large();        // 48 px — passkey, profile photo initials
const lv_font_t* font_clock();        // 48 px — base font for the clock; UI scales 2x

// Apply default page styling (background color, no padding, no border) to
// a top-level screen object.
void apply_to_screen(lv_obj_t* screen);

// Helper — converts the uint32_t hex tokens above to lv_color_t.
inline lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }

} // namespace theme
