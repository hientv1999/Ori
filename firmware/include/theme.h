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

// Connection-status palette for the profile-photo border — reflects Ori's
// own BLE connection state to Orion (Device Status, ble-protocol.md §3),
// derived on-device, not pushed over the wire (this replaced an earlier
// Teams-presence border; screen-layout.md). Also reused by
// modal_iphone_info.cpp as generic "good/neutral" status colors (battery,
// signal bars, connection dots) — unrelated to Ori's own connection state,
// just borrowing the same swatches.
//   RUNTIME_READY                        → green  (COLOR_CONN_CONNECTED)
//   RUNTIME_RECONNECTING / RUNTIME_SYNCING → amber (COLOR_CONN_SYNCING)
//   no BLE connection to Orion            → grey   (COLOR_CONN_DISCONNECTED)
constexpr uint32_t COLOR_CONN_CONNECTED      = 0x92C353;  // green
constexpr uint32_t COLOR_CONN_SYNCING        = 0xFFAA44;  // amber
constexpr uint32_t COLOR_CONN_DISCONNECTED   = 0x8A8884;  // grey
// (No separate LIGHT/DARK gradient stops — the ring is a flat fill of the
// colours above; the outward glow is a symmetric shadow, not a gradient. A
// top-light/bottom-dark vertical gradient was tried and reverted — it made
// the glow read as growing bottom-to-top instead of evenly outward from the
// photo.)

// Weather icon + temperature text palette (profile-photo overlay,
// `screen-layout.md` "Weather icon + temperature text"). Values are
// ported 1:1 from `WEATHER_ICONS` in Ori_UI_Prototype.js. The sun (Clear /
// PartlyCloudy) intentionally reuses COLOR_ACCENT instead of the prototype's
// near-duplicate gold (#F0B84C) — close enough to keep the palette tighter.
constexpr uint32_t COLOR_WEATHER_CLOUD_FRONT = 0xE7EAEE;  // front cloud fill — PartlyCloudy, Cloudy
constexpr uint32_t COLOR_WEATHER_CLOUD_BACK  = 0x9AA2AE;  // back/peeking cloud fill — Cloudy
constexpr uint32_t COLOR_WEATHER_CLOUD_LIGHT = 0xC7CDD6;  // cloud fill — Rain, Snow, and (as of the
                                                          // "thunderstorm day cloud isn't special" decision)
                                                          // Thunderstorm's day cloud too — only the bolt
                                                          // marks it a storm.
constexpr uint32_t COLOR_WEATHER_RAIN_DROP   = 0x5FB4E0;  // raindrop stroke — Rain
constexpr uint32_t COLOR_WEATHER_BOLT        = 0xF0C93E;  // lightning-bolt stroke — Thunderstorm
constexpr uint32_t COLOR_WEATHER_SNOW        = 0xDCEEFF;  // snowflake stroke — Snow
constexpr uint32_t COLOR_WEATHER_FOG         = 0xAAB2BD;  // fog-line stroke — Fog
constexpr uint32_t COLOR_WEATHER_TEMP_TEXT   = 0xFFFFFF;  // temperature text — white, no bubble background
constexpr uint32_t COLOR_WEATHER_MOON        = 0xB0B6C2;  // moon disc fill — Clear/PartlyCloudy at night
                                                          // (replaces the sun's rays entirely). A light,
                                                          // mostly-neutral grey with just a faint cool/blue
                                                          // hint (R/G/B within 18 of each other) — the
                                                          // earlier 0x6E86C4 read as too saturated/blue.
                                                          // Still meaningfully darker than Partly Cloudy's
                                                          // pale cloud fill (0xE7EAEE, ~55 points lighter)
                                                          // so the two don't blend at badge size, the same
                                                          // failure the original near-white 0xB9C4D6 had.
constexpr uint32_t COLOR_WEATHER_STAR        = 0x7B8494;  // background star dots — every condition at night

// Night-only cloud/fog tones — every cloud silhouette and the fog lines get a
// darker, cooler fill at night; precipitation (raindrops/snowflakes/bolts)
// keeps its day colour unchanged. Ported 1:1 from NIGHT_CLOUD in
// Ori_UI_Prototype.js. No day counterpart reuses these — each condition
// picks day vs. night fill at build time (widget_profile_card.cpp).
constexpr uint32_t COLOR_WEATHER_CLOUD_FRONT_NIGHT = 0x5A6272;  // night counterpart of CLOUD_FRONT
constexpr uint32_t COLOR_WEATHER_CLOUD_BACK_NIGHT  = 0x454C5C;  // night counterpart of CLOUD_BACK
constexpr uint32_t COLOR_WEATHER_CLOUD_LIGHT_NIGHT = 0x545C6E;  // night counterpart of CLOUD_LIGHT (Rain/Snow)
constexpr uint32_t COLOR_WEATHER_CLOUD_STORM_NIGHT = 0x3D4250;  // Thunderstorm's night-only darker tone —
                                                                // its day cloud now matches CLOUD_LIGHT,
                                                                // but night still darkens further than
                                                                // Rain/Snow's CLOUD_LIGHT_NIGHT.
constexpr uint32_t COLOR_WEATHER_FOG_NIGHT         = 0x4F5666;  // night counterpart of FOG

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
