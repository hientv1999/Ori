#include "widgets/widget_profile_card.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "screens/modal_factory_reset.h"
#include "screens/modal_profile.h"
#include "theme.h"

// LVGL has no native multi-stop gradient; bg_grad_dir = VER with two colour
// stops approximates the prototype's vertical linear-gradient closely enough.

namespace {

struct CardState {
    lv_obj_t* photo_ring;   // gradient ring — PHOTO_SIZE+12 circle, bg is the gradient border
    lv_obj_t* photo;        // inner photo circle — PHOTO_SIZE, clip_corner, holds photo_img
    lv_obj_t* photo_img;    // lv_image_t child, hidden when no photo
    lv_obj_t* name_label;
    lv_obj_t* title_label;
    lv_obj_t* weather_badge;  // transparent icon container, top-left of photo_ring — condition glyph only, no bubble
    lv_obj_t* temp_label;     // plain text (no bubble), top-right of photo_ring, e.g. "-40°"/"72°"/"140°"
    bool suppress_click = false;
};

// Default connection status applied to newly-created cards. screen_manager
// sets this before each load() so the profile photo border colour stays
// consistent.
widget_profile_card::ConnStatus g_default_conn_status =
    widget_profile_card::ConnStatus::Disconnected;

// Default weather applied to newly-created cards — same pattern as
// g_default_conn_status. Freshly-booted devices (no weather ever received over
// BLE) must default to hidden, so the card never flashes a stale reading
// before the first Device Settings "w"/"n"/"i"/"d"/"u" write (ble-protocol.md §6.4).
widget_profile_card::WeatherCondition g_default_weather_condition =
    widget_profile_card::WeatherCondition::Clear;
int  g_default_weather_temp_f = 0;
widget_profile_card::TemperatureUnit g_default_weather_unit =
    widget_profile_card::TemperatureUnit::Fahrenheit;
bool g_default_weather_visible  = false;
bool g_default_weather_is_night = false;
widget_profile_card::WeatherIntensity g_default_weather_intensity =
    widget_profile_card::WeatherIntensity::None;

// Cached photo descriptor — set by set_photo() so new cards created after a
// photo arrives (e.g. screen transitions) start with the photo already loaded.
const lv_image_dsc_t* g_default_photo = nullptr;

// Weak reference to the most-recently-created card, used by set_photo() and
// set_profile() to update the live screen without a screen rebuild.
// Cleared when the card is deleted (LV_EVENT_DELETE handler).
lv_obj_t* g_active_card = nullptr;

// Optional live photo object registered by modal_profile. Receives border-colour
// updates alongside g_active_card so connection-status changes reflect
// immediately inside the overlay without closing and reopening it.
lv_obj_t* g_modal_photo = nullptr;

// Optional live photo IMAGE (lv_image) inside the modal_profile overlay. Updated
// by set_photo() so a photo arriving over BLE while the modal is open appears
// immediately. nullptr when the modal is closed.
lv_obj_t* g_modal_photo_img = nullptr;

// Live text-label handles for the modal_profile overlay. Populated by
// register_modal_labels(); zeroed on unregister_modal_labels().
widget_profile_card::ModalLabels g_modal_labels = {};

// Cached name, title, email, phone — populated from NVS at boot and updated on
// every BLE ProfileInfo write. create() reads these directly so the
// real synced values appear after the first setup sync.
// Field limits: name/title/email ≤ 32 chars, phone ≤ 16 chars (Orion enforces
// at input). Buffers hold the worst-case UTF-8 byte length (3 bytes/char for
// the scripts we ship, e.g. Vietnamese names) plus the NUL terminator.
char g_name[97]   = {};
char g_title[97]  = {};
char g_email[129] = {};
char g_phone[33]  = {};

// Returns g_name/g_title if set, otherwise an em-dash placeholder so the
// profile card never renders blank before a first sync.
static const char* display_name()  { return g_name[0]  ? g_name  : "No name"; }
static const char* display_title() { return g_title[0] ? g_title : "No position"; }

// ConnStatus enum values are 0..2 (widget_profile_card.h) — a table indexed
// by the raw value replaces a switch statement.
constexpr uint32_t CONN_STATUS_COLOR_TABLE[3] = {
    theme::COLOR_CONN_DISCONNECTED,
    theme::COLOR_CONN_SYNCING,
    theme::COLOR_CONN_CONNECTED,
};

// Out-of-range values (shouldn't happen — ConnStatus is an enum class with
// only these 3 values) fall back to Disconnected, matching the switch
// statement's old `default:` case.
uint32_t color_for_conn_status(widget_profile_card::ConnStatus s) {
    uint8_t idx = static_cast<uint8_t>(s);
    return CONN_STATUS_COLOR_TABLE[idx < 3 ? idx : 0];
}

// Disconnected gets no glow — it's the "nothing to report" / fallback state,
// and a grey glow reads as a render glitch rather than a deliberate status
// signal.
lv_opa_t shadow_opa_for_conn_status(widget_profile_card::ConnStatus s) {
    return s == widget_profile_card::ConnStatus::Disconnected ? LV_OPA_TRANSP : LV_OPA_50;
}

// Applies the connection-status colour + glow to any status-ring-shaped
// object — shared by set_conn_status() (the live card's photo_ring) and
// set_default_conn_status() (the modal_profile overlay's own ring, when open).
void apply_conn_status_ring_style(lv_obj_t* ring, widget_profile_card::ConnStatus s) {
    if (!ring) return;
    lv_obj_set_style_bg_color(ring, theme::color(color_for_conn_status(s)), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(ring, theme::color(color_for_conn_status(s)), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(ring, shadow_opa_for_conn_status(s), LV_PART_MAIN);
}

// The name label's box is sized in whole lines (1 or 2) of font_time() so
// long names that wrap to a second line get room for it. Without this, a
// fixed 2-line box left a spare blank line above the job title whenever the
// name fit on one line — name and title looked too far apart. Sizing to the
// text's actual wrapped line count closes that gap for short names while
// still expanding for long ones.
void fit_name_label_height(lv_obj_t* label, const char* text) {
    const lv_font_t* font = theme::font_time();
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0,
                      widget_profile_card::WIDTH - 16, LV_TEXT_FLAG_NONE);
    int32_t lines = (size.y + font->line_height / 2) / font->line_height;
    if (lines < 1) lines = 1;
    if (lines > 2) lines = 2;
    lv_obj_set_height(label, lines * font->line_height);
}

// ── Weather icon glyph construction ─────────────────────────────────────────
//
// Ports `WEATHER_ICONS` / `cloudSvg()` in Ori_UI_Prototype.js — every glyph
// is built only from circles, rounded rects, and straight-line strokes (no
// bezier art), so each SVG shape maps 1:1 to a stacked lv_obj/lv_line. Source
// coordinates are the prototype's 32x32 SVG viewBox; ICON_SCALE reproduces
// the prototype's "content inside the badge" ratio, and ICON_OFFSET centers
// that icon area inside the (now fully transparent) container.
constexpr int16_t BADGE_SIZE   = 60;   // transparent icon container — sizes the glyph's coordinate space only, no bubble drawn
constexpr float   ICON_SCALE   = 42.0f / 32.0f;  // 42px glyph content (was 28, ×1.5)
// Centre the 42px icon in the container (no border to inset for anymore).
constexpr int16_t ICON_OFFSET  = (BADGE_SIZE - 42) / 2;

// Offset from photo_ring's centre for the weather icon (top-left) and
// temperature text (top-right), in raw lv_obj_align units. photo_ring's
// radius is (widget_profile_card::PHOTO_SIZE + 12) / 2 = 120 px. X/Y differ
// (Y is 10 px further out) purely per visual preference — both already clear
// the ring's circle at equal X/Y, this just sits the pair higher above it.
// Clearance check (distance from ring centre to the FAR corner's content,
// minus its own half-diagonal, must exceed 120 + a few px of breathing room):
// the icon's glyph ink reaches ~18-21 px from its own centre, and the widest
// temp string "140°F"/"-40°C" (adv_w-summed per ori_font_hanken_24.c) has a
// ~33 px half-diagonal — sqrt(110² + 120²) ≈ 163 px clears both with margin.
// (temp_label additionally shifts in by TEMP_LABEL_SHIFT_X below, which only
// tightens that margin — sqrt(100² + 120²) ≈ 156 px still clears the 120 px
// ring radius comfortably.)
constexpr int16_t CORNER_OFFSET_X = 110;
constexpr int16_t CORNER_OFFSET_Y = 120;

// Temperature text sits close enough to CORNER_OFFSET_X that its degree/unit
// suffix crowds the right screen edge. Shift it left independently of
// CORNER_OFFSET_X, which the weather icon on the opposite corner also uses.
constexpr int16_t TEMP_LABEL_SHIFT_X = 10;

// Scales a length/radius/stroke-width (no offset). Floors at 1px so thin
// prototype strokes (e.g. 1.3px snowflakes) stay visible at this size.
int16_t wsc(float v) {
    int16_t r = static_cast<int16_t>(lroundf(v * ICON_SCALE));
    return r < 1 ? 1 : r;
}

// Scales + offsets a coordinate into badge-local pixel space.
int16_t wix(float v) {
    return static_cast<int16_t>(ICON_OFFSET + lroundf(v * ICON_SCALE));
}

lv_obj_t* add_wcircle(lv_obj_t* parent, float cx, float cy, float r, uint32_t color) {
    int16_t d = wsc(r * 2);
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, wix(cx) - d / 2, wix(cy) - d / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, theme::color(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* add_wrect(lv_obj_t* parent, float x, float y, float w, float h,
                     float radius, uint32_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, wsc(w), wsc(h));
    lv_obj_set_pos(o, wix(x), wix(y));
    lv_obj_set_style_radius(o, wsc(radius), 0);
    lv_obj_set_style_bg_color(o, theme::color(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// Multi-point polyline stroke (2 points = a plain segment, 4 = the
// thunderbolt). Points are heap-allocated because lv_line only stores the
// pointer it's given (no copy) — freed on the line's own LV_EVENT_DELETE,
// which fires both when the badge is lv_obj_clean()'d on a weather update
// and when the whole card is torn down.
void add_wline(lv_obj_t* parent, std::initializer_list<float> coords,
               uint32_t color, float stroke_w) {
    size_t n = coords.size() / 2;
    auto* pts = new lv_point_precise_t[n];
    size_t i = 0;
    for (auto it = coords.begin(); it != coords.end(); ) {
        float x = *it++;
        float y = *it++;
        pts[i].x = wix(x);
        pts[i].y = wix(y);
        i++;
    }
    lv_obj_t* ln = lv_line_create(parent);
    lv_line_set_points(ln, pts, static_cast<uint16_t>(n));
    lv_obj_set_style_line_color(ln, theme::color(color), 0);
    lv_obj_set_style_line_width(ln, wsc(stroke_w), 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    lv_obj_set_pos(ln, 0, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ln, [](lv_event_t* e) {
        delete[] static_cast<lv_point_precise_t*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, pts);
}

// Shared cloud silhouette — rounded-rect base + 3 overlapping circles,
// drawn base-then-circles so the circles paint on top (matches cloudSvg()'s
// SVG paint order). dx/dy shift the whole shape (partly-cloudy offsets it to
// make room for the sun).
void add_wcloud(lv_obj_t* parent, uint32_t fill, float dx = 0, float dy = 0) {
    add_wrect(parent, 3 + dx, 13 + dy, 20, 9, 4.5f, fill);
    add_wcircle(parent, 8.5f + dx, 12 + dy, 5, fill);
    add_wcircle(parent, 14 + dx, 9 + dy, 6.3f, fill);
    add_wcircle(parent, 19.5f + dx, 12.5f + dy, 5, fill);
}

// Night-sky dots — ports starsSvg()/STAR_POSITIONS in Ori_UI_Prototype.js.
// Drawn before the cloud/sun/precipitation shapes so a star that happens to
// land under a cloud's edge is naturally covered rather than poking through.
struct StarPt { float cx, cy, r; };
void add_wstars(lv_obj_t* parent, const StarPt* pts, size_t n) {
    for (size_t i = 0; i < n; i++) {
        add_wcircle(parent, pts[i].cx, pts[i].cy, pts[i].r, theme::COLOR_WEATHER_STAR);
    }
}
constexpr StarPt STARS_CLEAR[]         = {{5, 5, 0.9f}, {27, 6, 0.7f}, {6, 27, 0.6f}, {26, 26, 0.8f}};
constexpr StarPt STARS_PARTLY_CLOUDY[] = {{4, 4, 0.7f}, {28, 5, 0.6f}};
constexpr StarPt STARS_CLOUDY[]        = {{2.5f, 6, 0.7f}, {2.5f, 27, 0.6f}};
constexpr StarPt STARS_CLOUD_TOP[]     = {{3, 4, 0.7f}, {29, 4, 0.6f}};  // Rain/Thunderstorm/Snow
constexpr StarPt STARS_FOG[]           = {{4, 4, 0.6f}, {28, 4, 0.6f}};

// Rain: ports rainDropsSvg() — count/length/stroke scale with intensity,
// always centred on x=16 regardless of count so the icon doesn't drift.
void add_wraindrops(lv_obj_t* parent, int count, float len, float stroke_w) {
    const float spacing = 6, start_x = 16 - ((count - 1) * spacing) / 2;
    for (int i = 0; i < count; i++) {
        float x = start_x + i * spacing;
        add_wline(parent, {x, 24, x - 2, 24 + len}, theme::COLOR_WEATHER_RAIN_DROP, stroke_w);
    }
}

// Snow: ports snowflakeSvg()/snowflakesSvg() — one flake is a 3-line asterisk
// through a shared centre point; count scales with intensity, size does not.
void add_wsnowflake(lv_obj_t* parent, float cx, float stroke_w) {
    const float cy = 26, vlen = 2.4f, dx = 2.2f, dy = 1.4f;
    const uint32_t snow = theme::COLOR_WEATHER_SNOW;
    add_wline(parent, {cx, cy - vlen, cx, cy + vlen}, snow, stroke_w);
    add_wline(parent, {cx - dx, cy - dy, cx + dx, cy + dy}, snow, stroke_w);
    add_wline(parent, {cx + dx, cy - dy, cx - dx, cy + dy}, snow, stroke_w);
}
void add_wsnowflakes(lv_obj_t* parent, int count, float stroke_w) {
    const float spacing = 7, start_x = 16 - ((count - 1) * spacing) / 2;
    for (int i = 0; i < count; i++) add_wsnowflake(parent, start_x + i * spacing, stroke_w);
}

// Thunderbolt: ports boltSvg() — scaled/shifted from its own top anchor point
// so Heavy can draw two side by side instead of one enlarged bolt.
void add_wbolt(lv_obj_t* parent, float scale, float dx, float stroke_w) {
    // Shifted -2.5 in x from the original {{18.5,20},{14,26.5},{17,26.5},{13.5,31.5}}
    // so the bolt's top/anchor point (x=16) sits under the cloud's own centre
    // instead of visibly right of it.
    constexpr float base[4][2] = {{16, 20}, {11.5f, 26.5f}, {14.5f, 26.5f}, {11, 31.5f}};
    const float ax = base[0][0], ay = base[0][1];
    add_wline(parent, {
        ax + (base[0][0] - ax) * scale + dx, ay + (base[0][1] - ay) * scale,
        ax + (base[1][0] - ax) * scale + dx, ay + (base[1][1] - ay) * scale,
        ax + (base[2][0] - ax) * scale + dx, ay + (base[2][1] - ay) * scale,
        ax + (base[3][0] - ax) * scale + dx, ay + (base[3][1] - ay) * scale,
    }, theme::COLOR_WEATHER_BOLT, stroke_w);
}

// Fog: ports fogLinesSvg() — Light is a couple of short, thin wisps; Heavy is
// the original dense 4-band pattern (unchanged from the pre-intensity icon).
void add_wfog(lv_obj_t* parent, bool heavy, bool is_night) {
    const uint32_t fog = is_night ? theme::COLOR_WEATHER_FOG_NIGHT : theme::COLOR_WEATHER_FOG;
    const float stroke_w = heavy ? 2.2f : 1.6f;
    if (heavy) {
        add_wline(parent, {5, 7.5f, 27, 7.5f}, fog, stroke_w);
        add_wline(parent, {8, 13.5f, 27, 13.5f}, fog, stroke_w);
        add_wline(parent, {5, 19.5f, 24, 19.5f}, fog, stroke_w);
        add_wline(parent, {9, 24.5f, 27, 24.5f}, fog, stroke_w);
    } else {
        add_wline(parent, {7, 12, 23, 12}, fog, stroke_w);
        add_wline(parent, {9, 20, 25, 20}, fog, stroke_w);
    }
}

void build_weather_icon(lv_obj_t* badge, widget_profile_card::WeatherCondition cond,
                         bool is_night, widget_profile_card::WeatherIntensity intensity) {
    using WC = widget_profile_card::WeatherCondition;
    using WI = widget_profile_card::WeatherIntensity;
    switch (cond) {
        case WC::Clear: {
            if (is_night) {
                add_wstars(badge, STARS_CLEAR, sizeof(STARS_CLEAR) / sizeof(StarPt));
                add_wcircle(badge, 16, 16, 7.5f, theme::COLOR_WEATHER_MOON);
                break;
            }
            const uint32_t sun = theme::COLOR_ACCENT;
            add_wline(badge, {16, 2, 16, 6}, sun, 1.8f);
            add_wline(badge, {16, 26, 16, 30}, sun, 1.8f);
            add_wline(badge, {2, 16, 6, 16}, sun, 1.8f);
            add_wline(badge, {26, 16, 30, 16}, sun, 1.8f);
            add_wline(badge, {6.3f, 6.3f, 9.1f, 9.1f}, sun, 1.8f);
            add_wline(badge, {22.9f, 22.9f, 25.7f, 25.7f}, sun, 1.8f);
            add_wline(badge, {6.3f, 25.7f, 9.1f, 22.9f}, sun, 1.8f);
            add_wline(badge, {22.9f, 9.1f, 25.7f, 6.3f}, sun, 1.8f);
            add_wcircle(badge, 16, 16, 7.5f, sun);
            break;
        }
        case WC::PartlyCloudy: {
            if (is_night) {
                add_wstars(badge, STARS_PARTLY_CLOUDY, sizeof(STARS_PARTLY_CLOUDY) / sizeof(StarPt));
                add_wcircle(badge, 8, 8, 5.2f, theme::COLOR_WEATHER_MOON);
            } else {
                const uint32_t sun = theme::COLOR_ACCENT;
                add_wline(badge, {8, 0.3f, 8, 2.6f}, sun, 1.6f);
                add_wline(badge, {0.8f, 1, 2.7f, 2.8f}, sun, 1.6f);
                add_wline(badge, {0.3f, 8, 2.6f, 8}, sun, 1.6f);
                add_wcircle(badge, 8, 8, 5.2f, sun);
            }
            // Cloud painted last (on top), partially covering the sun/moon.
            add_wcloud(badge, is_night ? theme::COLOR_WEATHER_CLOUD_FRONT_NIGHT
                                        : theme::COLOR_WEATHER_CLOUD_FRONT, 3, 2);
            break;
        }
        case WC::Cloudy: {
            if (is_night) add_wstars(badge, STARS_CLOUDY, sizeof(STARS_CLOUDY) / sizeof(StarPt));
            // Whole composition nudged right 2.25 units to centre in the badge
            // (front + back cloud shifted equally so their overlap is preserved).
            // Smaller "back" cloud peeking up-right behind the front cloud.
            const uint32_t back = is_night ? theme::COLOR_WEATHER_CLOUD_BACK_NIGHT
                                            : theme::COLOR_WEATHER_CLOUD_BACK;
            add_wrect(badge, 14.25f, 8, 14, 6.5f, 3.2f, back);
            add_wcircle(badge, 18.25f, 7.3f, 3.6f, back);
            add_wcircle(badge, 21.95f, 5.4f, 4.4f, back);
            add_wcircle(badge, 25.55f, 7.6f, 3.6f, back);
            add_wcloud(badge, is_night ? theme::COLOR_WEATHER_CLOUD_FRONT_NIGHT
                                        : theme::COLOR_WEATHER_CLOUD_FRONT, 2.25f, 0);
            break;
        }
        case WC::Rain: {
            if (is_night) add_wstars(badge, STARS_CLOUD_TOP, sizeof(STARS_CLOUD_TOP) / sizeof(StarPt));
            // Cloud nudged right ~2.25 units so its silhouette centres in the
            // badge; raindrops below are deliberately left where they are.
            add_wcloud(badge, is_night ? theme::COLOR_WEATHER_CLOUD_LIGHT_NIGHT
                                        : theme::COLOR_WEATHER_CLOUD_LIGHT, 2.25f, 0);
            switch (intensity) {
                case WI::Light: add_wraindrops(badge, 1, 4, 1.5f); break;
                case WI::Heavy: add_wraindrops(badge, 3, 6, 2.2f); break;
                default:        add_wraindrops(badge, 2, 5, 1.8f); break;  // Moderate/None
            }
            break;
        }
        case WC::Thunderstorm: {
            if (is_night) add_wstars(badge, STARS_CLOUD_TOP, sizeof(STARS_CLOUD_TOP) / sizeof(StarPt));
            // Cloud nudged right to centre; bolt(s) below left where they are.
            // Day cloud uses the same regular light-grey fill as Rain/Snow (not
            // a special darker "storm" tone) — only the bolt marks this a storm.
            // Night keeps a distinct, further-darkened tone.
            add_wcloud(badge, is_night ? theme::COLOR_WEATHER_CLOUD_STORM_NIGHT
                                        : theme::COLOR_WEATHER_CLOUD_LIGHT, 2.25f, 0);
            switch (intensity) {
                case WI::Light:
                    add_wbolt(badge, 0.72f, 0, 1.6f);
                    break;
                case WI::Heavy:
                    add_wbolt(badge, 0.85f, -5, 1.9f);
                    add_wbolt(badge, 0.85f, 0, 1.9f);
                    add_wbolt(badge, 0.85f, 5, 1.9f);
                    break;
                default:  // Moderate/None
                    add_wbolt(badge, 1, -3, 2.0f);
                    add_wbolt(badge, 1, 3, 2.0f);
                    break;
            }
            break;
        }
        case WC::Snow: {
            if (is_night) add_wstars(badge, STARS_CLOUD_TOP, sizeof(STARS_CLOUD_TOP) / sizeof(StarPt));
            // Cloud nudged right to centre; snowflakes below are left as-is.
            add_wcloud(badge, is_night ? theme::COLOR_WEATHER_CLOUD_LIGHT_NIGHT
                                        : theme::COLOR_WEATHER_CLOUD_LIGHT, 2.25f, 0);
            switch (intensity) {
                case WI::Light: add_wsnowflakes(badge, 1, 1.3f); break;
                case WI::Heavy: add_wsnowflakes(badge, 3, 1.3f); break;
                default:        add_wsnowflakes(badge, 2, 1.3f); break;
            }
            break;
        }
        case WC::Fog: {
            if (is_night) add_wstars(badge, STARS_FOG, sizeof(STARS_FOG) / sizeof(StarPt));
            // Lines shifted up 2.5 units — they spanned y 10..27 (centre 18.5),
            // a bit low; now y 7.5..24.5 centres them in the badge (Heavy).
            add_wfog(badge, intensity != WI::Light, is_night);
            break;
        }
    }
}

// Applies condition/temp_f/unit/visible/is_night/intensity to one already-built
// card's icon + text.
void apply_weather_to(CardState* s, widget_profile_card::WeatherCondition condition,
                       int temp_f, widget_profile_card::TemperatureUnit unit, bool visible,
                       bool is_night, widget_profile_card::WeatherIntensity intensity) {
    if (!s || !s->weather_badge || !s->temp_label) return;
    if (!visible) {
        lv_obj_add_flag(s->weather_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->temp_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clean(s->weather_badge);
    build_weather_icon(s->weather_badge, condition, is_night, intensity);
    char buf[10];
    char unit_ch = (unit == widget_profile_card::TemperatureUnit::Celsius) ? 'C' : 'F';
    // U+00B0 DEGREE SIGN (UTF-8) followed by the unit letter, e.g. "72\xC2\xB0F".
    snprintf(buf, sizeof(buf), "%d\xC2\xB0%c", temp_f, unit_ch);
    lv_label_set_text(s->temp_label, buf);

    // temp_label is aligned to (CORNER_OFFSET_X, CORNER_OFFSET_Y) off
    // photo_ring's centre (base position set in create(), re-applied here) —
    // but that centers the label's *advance-width* box, which isn't visually
    // symmetric: the font's per-glyph side bearings differ between the
    // leading character ("-" for negative values, or a digit for
    // positive/zero — digit "1" in particular has a much bigger left bearing
    // than the other digits, per ori_font_hanken_24.c) and the trailing unit
    // letter ("F"/"C", each with its own right bearing). Whenever those two
    // bearings differ, the box is positioned correctly but the visible ink is
    // not — most noticeable on short strings where 1-2 px is a bigger
    // fraction of the total width. Recompute the exact correction for THIS
    // string's actual leading/trailing glyphs instead of guessing a fixed
    // offset, and fold it into the same alignment call.
    lv_font_glyph_dsc_t g_first{}, g_last{};
    int32_t left_bearing = 0, right_bearing = 0;
    if (lv_font_get_glyph_dsc(theme::font_meta(), &g_first, (uint32_t)(unsigned char)buf[0], 0)) {
        left_bearing = g_first.ofs_x;
    }
    if (lv_font_get_glyph_dsc(theme::font_meta(), &g_last, (uint32_t)(unsigned char)unit_ch, 0)) {
        right_bearing = (int32_t)g_last.adv_w - g_last.ofs_x - (int32_t)g_last.box_w;
    }
    lv_obj_align(s->temp_label, LV_ALIGN_CENTER,
                 CORNER_OFFSET_X - TEMP_LABEL_SHIFT_X + (right_bearing - left_bearing) / 2,
                 -CORNER_OFFSET_Y);

    lv_obj_clear_flag(s->weather_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s->temp_label, LV_OBJ_FLAG_HIDDEN);
}

} // namespace

namespace widget_profile_card {

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* card = lv_obj_create(parent);
    // Use lv_pct(100) for height — lv_obj_get_height(parent) returns 0 when
    // called before LVGL has resolved the parent's layout, which collapses
    // the card to zero height (nothing rendered). lv_pct(100) defers to
    // the parent's content area at draw time and works correctly inside
    // the body's flex-row layout.
    lv_obj_set_size(card, WIDTH, lv_pct(100));

    // Vertical gradient: COLOR_BG (top) -> 0x0B0E13 (bottom).
    lv_obj_set_style_bg_color(card, theme::color(theme::COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(card, theme::color(0x0B0E13), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(card, 8, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // OVERFLOW_VISIBLE so the connection-status ring's glow (drawn outside
    // the ring's own box) isn't clipped to the card bounds — LVGL clips a
    // child's draw, including its shadow, to its parent's box by default.
    lv_obj_add_flag(card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* s = new CardState();
    s->photo_img = nullptr;

    // Track the live card so set_photo() can update it without a screen rebuild.
    g_active_card = card;

    // ── Connection-status ring ─────────────────────────────────────────────────
    // A circle 12 px larger than the photo (PHOTO_SIZE + 12 = 6 px each side).
    // Its solid background IS the "border" — a single flat connection-status
    // colour. (Previously a top-light/bottom-dark vertical gradient, which
    // made the ring — and the glow together with it — read as growing
    // bottom-to-top instead of radiating evenly outward from the photo; a
    // flat fill lets the shadow below be the sole "glow" cue, symmetric in
    // every direction.)
    // Tap / long-press events are on the inner photo; the ring is non-clickable.
    s->photo_ring = lv_obj_create(card);
    lv_obj_set_size(s->photo_ring, PHOTO_SIZE + 12, PHOTO_SIZE + 12);
    lv_obj_set_style_radius(s->photo_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->photo_ring,
        theme::color(color_for_conn_status(g_default_conn_status)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->photo_ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->photo_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->photo_ring, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s->photo_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->photo_ring, LV_OBJ_FLAG_CLICKABLE);

    // Static connection-status glow — a fixed (non-animated) soft shadow in
    // the status colour, sitting outside the ring. Offset (0,0) + spread
    // makes it radiate evenly outward from the ring's edge in every
    // direction — the "grows from inside the circle to outward" effect. No
    // breathing/pulse anim, unlike the setup-screen button glow in ui_helpers.cpp.
    lv_obj_set_style_shadow_color(s->photo_ring,
        theme::color(color_for_conn_status(g_default_conn_status)), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s->photo_ring, 40, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(s->photo_ring, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s->photo_ring, shadow_opa_for_conn_status(g_default_conn_status), LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_x(s->photo_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s->photo_ring, 0, LV_PART_MAIN);
    // The weather icon + temperature text (added below) sit outside photo_ring's
    // own box (CORNER_OFFSET_X/Y clears the ring's circle, which exceeds the
    // box on the diagonal) — up to ~30 px for weather_badge's own (mostly
    // empty/transparent) 60px container, though its actual glyph ink stays
    // well inside that. LVGL clips a child's draw to its parent's box by
    // default; OVERFLOW_VISIBLE here relaxes that clip by photo_ring's own
    // ext_draw_size — exactly shadow_width/2+1+shadow_spread = 40/2+1+8 = 29 px
    // for the shadow set below — comfortably covering the actually-visible
    // content even where the invisible container box overhangs slightly
    // further. Same mechanism as `card`'s flag above,
    // one level down the tree.
    lv_obj_add_flag(s->photo_ring, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // ── Inner photo circle ────────────────────────────────────────────────────
    // The connection-status ring peeks 6 px around all edges of this circle.
    // Tap opens the profile detail overlay; long-press (3 s) opens factory reset.
    s->photo = lv_obj_create(s->photo_ring);
    lv_obj_set_size(s->photo, PHOTO_SIZE, PHOTO_SIZE);
    lv_obj_center(s->photo);
    lv_obj_set_style_radius(s->photo, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->photo, theme::color(0x2A3140), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s->photo, theme::color(0x1A1F28), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s->photo, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->photo, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->photo, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->photo, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s->photo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s->photo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(s->photo, LV_OPA_60, LV_STATE_PRESSED);
    // Store CardState in user data for event handlers
    lv_obj_set_user_data(s->photo, s);
    lv_obj_add_event_cb(s->photo, [](lv_event_t* e) {
        auto* s = static_cast<CardState*>(lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e)));
        if (s && s->suppress_click) {
            s->suppress_click = false;
            return;
        }
        // Pass photo_ring so modal_profile aligns its ring to the same position.
        modal_profile::create(lv_screen_active(), s ? s->photo_ring : nullptr);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s->photo, [](lv_event_t* e) {
        auto* s = static_cast<CardState*>(lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e)));
        if (s) s->suppress_click = true;
        modal_factory_reset::create(lv_screen_active());
    }, LV_EVENT_LONG_PRESSED, nullptr);

    // Clip photo image to the circular boundary.
    lv_obj_set_style_clip_corner(s->photo, true, LV_PART_MAIN);

    // Photo image — hidden initially; shown when a decoded JPEG is available.
    // Sized to fill the container so the circle clip masks it correctly.
    s->photo_img = lv_image_create(s->photo);
    lv_obj_set_size(s->photo_img, PHOTO_SIZE, PHOTO_SIZE);
    lv_obj_align(s->photo_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);

    // Apply a cached photo immediately if one was already received before
    // this card was created (e.g. boot with a LittleFS-cached photo, or
    // screen transition after photo arrived).
    if (g_default_photo) {
        lv_image_set_src(s->photo_img, g_default_photo);
        lv_obj_clear_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);
    }

    // ── Weather icon ──────────────────────────────────────────────────────────
    // No bubble/background — a transparent container that just holds and
    // positions the condition glyph (see build_weather_icon()), sitting fully
    // off the top-left corner of photo_ring (never overlapping the photo or
    // its connection-status ring — see CORNER_OFFSET_X/Y). Children of this container
    // are rebuilt from scratch on every set_weather() call — cheap, since
    // weather changes at most every 15-30 min (ble-protocol.md §6.3).
    s->weather_badge = lv_obj_create(s->photo_ring);
    lv_obj_set_size(s->weather_badge, BADGE_SIZE, BADGE_SIZE);
    lv_obj_set_style_bg_opa(s->weather_badge, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->weather_badge, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->weather_badge, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s->weather_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->weather_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s->weather_badge, LV_ALIGN_CENTER, -CORNER_OFFSET_X, -CORNER_OFFSET_Y);
    lv_obj_add_flag(s->weather_badge, LV_OBJ_FLAG_HIDDEN);

    // ── Temperature text ──────────────────────────────────────────────────────
    // No bubble/background — plain text (COLOR_WEATHER_TEMP_TEXT, white) sits
    // directly on the card, fully off the top-right corner of photo_ring
    // (same CORNER_OFFSET_X/Y clearance as the weather icon above). font_meta()
    // @ 24px. Valid range is -40..140 (ble-protocol.md §10).
    s->temp_label = lv_label_create(s->photo_ring);
    lv_label_set_text(s->temp_label, "");
    lv_obj_set_style_text_font(s->temp_label, theme::font_meta(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s->temp_label, theme::color(theme::COLOR_WEATHER_TEMP_TEXT), LV_PART_MAIN);
    lv_obj_clear_flag(s->temp_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->temp_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s->temp_label, LV_OBJ_FLAG_HIDDEN);

    // Apply whatever weather was last received (or the hidden default on a
    // freshly-booted device — see g_default_weather_visible). Also sets
    // temp_label's actual position (CORNER_OFFSET_X/Y + bearing correction).
    apply_weather_to(s, g_default_weather_condition, g_default_weather_temp_f,
                      g_default_weather_unit, g_default_weather_visible,
                      g_default_weather_is_night, g_default_weather_intensity);

    // Name. Uses font_time() (30 px). Single line with ellipsis per
    // screen-layout.md ("Full name — single line, ellipsis on overflow").
    // Orion enforces name ≤ 32 chars at input so truncation is rare.
    s->name_label = lv_label_create(card);
    lv_label_set_long_mode(s->name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s->name_label, WIDTH - 16);
    lv_label_set_text(s->name_label, display_name());
    fit_name_label_height(s->name_label, display_name());
    lv_obj_set_style_text_color(s->name_label, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s->name_label, theme::font_time(), 0);
    lv_obj_set_style_text_align(s->name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s->name_label, 6, 0);

    // Title. Same single-line / ellipsis treatment; Orion limit ≤ 32 chars.
    // font_body() (20 px) matches the meeting-location and media-artist
    // sizes — the "secondary descriptor" tier across the device.
    s->title_label = lv_label_create(card);
    lv_label_set_text(s->title_label, display_title());
    lv_label_set_long_mode(s->title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s->title_label, WIDTH - 16);
    lv_obj_set_height(s->title_label, theme::font_meta()->line_height);
    lv_obj_set_style_text_color(s->title_label, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s->title_label, theme::font_meta(), 0);
    lv_obj_set_style_text_align(s->title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(s->title_label, 6, 0);

    lv_obj_set_user_data(card, s);
    lv_obj_add_event_cb(card, [](lv_event_t* e) {
        lv_obj_t* c = static_cast<lv_obj_t*>(lv_event_get_target(e));
        if (g_active_card == c) g_active_card = nullptr;
        delete static_cast<CardState*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, s);
    return card;
}

void set_conn_status(lv_obj_t* card, ConnStatus s_status) {
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(card));
    if (!s) return;
    apply_conn_status_ring_style(s->photo_ring, s_status);
}

void set_default_conn_status(ConnStatus s) {
    g_default_conn_status = s;
    if (g_active_card) set_conn_status(g_active_card, s);
    apply_conn_status_ring_style(g_modal_photo, s);
}

void register_modal_photo(lv_obj_t* photo_obj) { g_modal_photo = photo_obj; }
void unregister_modal_photo()                   { g_modal_photo = nullptr; }

void register_modal_photo_img(lv_obj_t* img_obj) { g_modal_photo_img = img_obj; }
void unregister_modal_photo_img()                { g_modal_photo_img = nullptr; }

void register_modal_labels(const ModalLabels& labels) { g_modal_labels = labels; }
void unregister_modal_labels()                         { g_modal_labels = {}; }

ConnStatus get_default_conn_status() {
    return g_default_conn_status;
}

uint32_t conn_status_color(ConnStatus s) { return color_for_conn_status(s); }

void set_weather(lv_obj_t* card, WeatherCondition condition, int temp_f,
                  TemperatureUnit unit, bool visible, bool is_night,
                  WeatherIntensity intensity) {
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(card));
    apply_weather_to(s, condition, temp_f, unit, visible, is_night, intensity);
}

void set_default_weather(WeatherCondition condition, int temp_f,
                          TemperatureUnit unit, bool visible, bool is_night,
                          WeatherIntensity intensity) {
    g_default_weather_condition = condition;
    g_default_weather_temp_f = temp_f;
    g_default_weather_unit = unit;
    g_default_weather_visible = visible;
    g_default_weather_is_night = is_night;
    g_default_weather_intensity = intensity;
    if (g_active_card) set_weather(g_active_card, condition, temp_f, unit, visible, is_night, intensity);
}

// Apply a photo descriptor to one image object: show it with the given source,
// or hide it when the descriptor is null.
static void apply_photo_to(lv_obj_t* img, const lv_image_dsc_t* img_dsc) {
    if (!img) return;
    if (img_dsc) {
        lv_image_set_src(img, img_dsc);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_photo(const lv_image_dsc_t* img_dsc) {
    // Store as the new default so future screen creates start with the photo.
    g_default_photo = img_dsc;

    // Update the currently visible card if one exists.
    if (g_active_card) {
        auto* s = static_cast<CardState*>(lv_obj_get_user_data(g_active_card));
        if (s) apply_photo_to(s->photo_img, img_dsc);
    }

    // Update the profile detail overlay's photo if it's open, so a photo
    // arriving over BLE while the modal is up appears without reopening it.
    apply_photo_to(g_modal_photo_img, img_dsc);
}

void set_profile(const char* name, const char* title,
                 const char* email, const char* phone) {
    if (name)  strncpy(g_name,  name,  sizeof(g_name)  - 1);
    if (title) strncpy(g_title, title, sizeof(g_title) - 1);
    if (email) strncpy(g_email, email, sizeof(g_email) - 1);
    if (phone) strncpy(g_phone, phone, sizeof(g_phone) - 1);

    // Update the live card labels if a card is currently on screen.
    if (g_active_card) {
        auto* s = static_cast<CardState*>(lv_obj_get_user_data(g_active_card));
        if (s) {
            if (s->name_label) {
                lv_label_set_text(s->name_label, display_name());
                fit_name_label_height(s->name_label, display_name());
            }
            if (s->title_label) lv_label_set_text(s->title_label, display_title());
        }
    }

    // Update the modal profile labels if the overlay is open.
    if (g_modal_labels.name_lbl)
        lv_label_set_text(g_modal_labels.name_lbl,  g_name[0]  ? g_name  : "No name");
    if (g_modal_labels.title_lbl)
        lv_label_set_text(g_modal_labels.title_lbl, g_title[0] ? g_title : "No position");
    if (g_modal_labels.email_lbl)
        lv_label_set_text(g_modal_labels.email_lbl, g_email[0] ? g_email : "No email");
    if (g_modal_labels.phone_lbl)
        lv_label_set_text(g_modal_labels.phone_lbl, g_phone[0] ? g_phone : "No phone number");
}

const char* get_profile_name()  { return g_name; }
const char* get_profile_title() { return g_title; }
const char* get_profile_email() { return g_email; }
const char* get_profile_phone() { return g_phone; }

} // namespace widget_profile_card
