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
constexpr uint32_t COLOR_ACCENT_FAINT     = 0x3E331D;   // accent at ~28% over black — current-week highlight
constexpr uint32_t COLOR_ACCENT_LINE      = 0x7B653A;   // accent at 55% over black
constexpr uint32_t COLOR_ACCENT_DARK      = 0x927845;   // accent at ~65% brightness — darker
                                                          // gold, used by the media-mode volume
                                                          // HUD bar fill (screen_media_mode.cpp)
constexpr uint32_t COLOR_DANGER           = 0xD86A6A;
constexpr uint32_t COLOR_DANGER_SOFT      = 0x1E0F0F;   // danger at 14% over black
constexpr uint32_t COLOR_OK               = 0x7FB48A;
// Solid yellow — the status-bar ANCS tile ring for a call still RINGING (not
// yet answered); COLOR_DANGER is reused for the same ring once the call goes
// ACTIVE (answered). Orion mirrors this exact pair (styles.css's --call-ring-*
// custom properties) so the ringing/ongoing call chip reads identically on
// both screens (pc-app.md).
constexpr uint32_t COLOR_CALL_RINGING     = 0xF2C94C;
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
// (No separate LIGHT/DARK gradient stops — the presence ring is a flat fill
// of the colours above; the outward glow is a symmetric shadow, not a
// gradient. A top-light/bottom-dark vertical gradient was tried and reverted
// — it made the glow read as growing bottom-to-top instead of evenly
// outward from the photo.)

// Weather icon + temperature text palette (profile-photo overlay,
// `screen-layout.md` "Weather icon + temperature text"). Values are
// ported 1:1 from `WEATHER_ICONS` in Ori_UI_Prototype.js. The sun (Clear /
// PartlyCloudy) intentionally reuses COLOR_ACCENT instead of the prototype's
// near-duplicate gold (#F0B84C) — close enough to keep the palette tighter.
constexpr uint32_t COLOR_WEATHER_CLOUD_FRONT = 0xE7EAEE;  // front cloud fill — PartlyCloudy, Cloudy
constexpr uint32_t COLOR_WEATHER_CLOUD_BACK  = 0x9AA2AE;  // back/peeking cloud fill — Cloudy
constexpr uint32_t COLOR_WEATHER_CLOUD_LIGHT = 0xC7CDD6;  // cloud fill — Rain, Snow
constexpr uint32_t COLOR_WEATHER_CLOUD_STORM = 0x8B93A1;  // cloud fill — Thunderstorm
constexpr uint32_t COLOR_WEATHER_RAIN_DROP   = 0x5FB4E0;  // raindrop stroke — Rain
constexpr uint32_t COLOR_WEATHER_BOLT        = 0xF0C93E;  // lightning-bolt stroke — Thunderstorm
constexpr uint32_t COLOR_WEATHER_SNOW        = 0xDCEEFF;  // snowflake stroke — Snow
constexpr uint32_t COLOR_WEATHER_FOG         = 0xAAB2BD;  // fog-line stroke — Fog
constexpr uint32_t COLOR_WEATHER_TEMP_TEXT   = 0xFFFFFF;  // temperature text — white, no bubble background

// Font getters — return pointers to Hanken Grotesk Medium fonts.
// Sizes were selected to mirror the prototype's CSS scale.
const lv_font_t* font_body();         // 20 px — default text
const lv_font_t* font_meta();         // 24 px — meeting meta line, status bar date/sep, calendar day numbers
const lv_font_t* font_title();        // 26 px — meeting title, headings
const lv_font_t* font_h2();           // 28 px — setup section headings
const lv_font_t* font_time();         // 30 px — status bar time
const lv_font_t* font_display();      // 42 px — empty state headline / setup welcome
const lv_font_t* font_large();        // 48 px — passkey modal
const lv_font_t* font_clock_xl();      // 96 px, digits + ':' + '-' only — clock face hour/minute
const lv_font_t* font_wordmark_xl();   // 90 px, glyphs 'o'/'r'/'i'/space ONLY — boot splash wordmark

// Build the color-emoji fallback and attach it to the user-text fonts
// (font_body/meta/title/h2). Call once at boot after lv_init(). Safe no-op if
// the emoji set is empty. Until called, those getters return plain Hanken.
void init_emoji_fallback();

// Apply default page styling (background color, no padding, no border) to
// a top-level screen object.
void apply_to_screen(lv_obj_t* screen);

// Helper — converts the uint32_t hex tokens above to lv_color_t.
inline lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }

} // namespace theme
