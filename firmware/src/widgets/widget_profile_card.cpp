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
    lv_obj_t* weather_badge;  // 46px circle, top-left of photo_ring — condition glyph
    lv_obj_t* temp_bubble;    // fixed 60px white circle, bottom-right of photo_ring
    lv_obj_t* temp_label;     // text child of temp_bubble, e.g. "-40°"/"72°"/"140°"
    bool suppress_click = false;
};

// Default presence applied to newly-created cards. screen_manager sets
// this before each load() so the profile photo border colour stays consistent.
widget_profile_card::Presence g_default_presence =
    widget_profile_card::Presence::Offline;

// Default weather applied to newly-created cards — same pattern as
// g_default_presence. Freshly-booted devices (no weather ever received over
// BLE) must default to hidden, so the card never flashes a stale reading
// before the first Device Settings "w"/"d" write (ble-protocol.md §6.4).
widget_profile_card::WeatherCondition g_default_weather_condition =
    widget_profile_card::WeatherCondition::Clear;
int  g_default_weather_temp_f = 0;
bool g_default_weather_visible = false;

// Cached photo descriptor — set by set_photo() so new cards created after a
// photo arrives (e.g. screen transitions) start with the photo already loaded.
const lv_image_dsc_t* g_default_photo = nullptr;

// Weak reference to the most-recently-created card, used by set_photo() and
// set_profile() to update the live screen without a screen rebuild.
// Cleared when the card is deleted (LV_EVENT_DELETE handler).
lv_obj_t* g_active_card = nullptr;

// Optional live photo object registered by modal_profile. Receives border-colour
// updates alongside g_active_card so presence toggles reflect immediately inside
// the overlay without closing and reopening it.
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

uint32_t color_for_presence(widget_profile_card::Presence p) {
    switch (p) {
        case widget_profile_card::Presence::Available: return theme::COLOR_PRESENCE_AVAILABLE;
        case widget_profile_card::Presence::Busy:      return theme::COLOR_PRESENCE_BUSY;
        case widget_profile_card::Presence::Away:      return theme::COLOR_PRESENCE_AWAY;
        case widget_profile_card::Presence::Offline:
        default:                                       return theme::COLOR_PRESENCE_OFFLINE;
    }
}

// Top gradient stop — near-white pastel tint of the presence colour.
uint32_t color_for_presence_light(widget_profile_card::Presence p) {
    switch (p) {
        case widget_profile_card::Presence::Available: return theme::COLOR_PRESENCE_AVAILABLE_LIGHT;
        case widget_profile_card::Presence::Busy:      return theme::COLOR_PRESENCE_BUSY_LIGHT;
        case widget_profile_card::Presence::Away:      return theme::COLOR_PRESENCE_AWAY_LIGHT;
        case widget_profile_card::Presence::Offline:
        default:                                       return theme::COLOR_PRESENCE_OFFLINE_LIGHT;
    }
}

// Bottom gradient stop — deep dark shade of the presence colour.
uint32_t color_for_presence_dark(widget_profile_card::Presence p) {
    switch (p) {
        case widget_profile_card::Presence::Available: return theme::COLOR_PRESENCE_AVAILABLE_DARK;
        case widget_profile_card::Presence::Busy:      return theme::COLOR_PRESENCE_BUSY_DARK;
        case widget_profile_card::Presence::Away:      return theme::COLOR_PRESENCE_AWAY_DARK;
        case widget_profile_card::Presence::Offline:
        default:                                       return theme::COLOR_PRESENCE_OFFLINE_DARK;
    }
}

// Offline gets no glow — it's the "nothing to report" / fallback state, and a
// grey glow reads as a render glitch rather than a deliberate status signal.
lv_opa_t shadow_opa_for_presence(widget_profile_card::Presence p) {
    return p == widget_profile_card::Presence::Offline ? LV_OPA_TRANSP : LV_OPA_50;
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

// ── Weather badge glyph construction ────────────────────────────────────────
//
// Ports `WEATHER_ICONS` / `cloudSvg()` in Ori_UI_Prototype.js — every glyph
// is built only from circles, rounded rects, and straight-line strokes (no
// bezier art), so each SVG shape maps 1:1 to a stacked lv_obj/lv_line. Source
// coordinates are the prototype's 32x32 SVG viewBox; ICON_SCALE reproduces
// the prototype's "28x28 content inside a 46px badge" ratio, and ICON_OFFSET
// centers that 28x28 icon area inside the badge circle.
constexpr float ICON_SCALE  = 28.0f / 32.0f;
constexpr int16_t ICON_OFFSET = 9;  // (46 - 28) / 2

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

void build_weather_icon(lv_obj_t* badge, widget_profile_card::WeatherCondition cond) {
    using WC = widget_profile_card::WeatherCondition;
    switch (cond) {
        case WC::Clear: {
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
            const uint32_t sun = theme::COLOR_ACCENT;
            add_wline(badge, {8, 0.3f, 8, 2.6f}, sun, 1.6f);
            add_wline(badge, {0.8f, 1, 2.7f, 2.8f}, sun, 1.6f);
            add_wline(badge, {0.3f, 8, 2.6f, 8}, sun, 1.6f);
            add_wcircle(badge, 8, 8, 5.2f, sun);
            // Cloud painted last (on top), partially covering the sun.
            add_wcloud(badge, theme::COLOR_WEATHER_CLOUD_FRONT, 3, 2);
            break;
        }
        case WC::Cloudy: {
            // Smaller "back" cloud peeking up-right behind the front cloud.
            add_wrect(badge, 12, 8, 14, 6.5f, 3.2f, theme::COLOR_WEATHER_CLOUD_BACK);
            add_wcircle(badge, 16, 7.3f, 3.6f, theme::COLOR_WEATHER_CLOUD_BACK);
            add_wcircle(badge, 19.7f, 5.4f, 4.4f, theme::COLOR_WEATHER_CLOUD_BACK);
            add_wcircle(badge, 23.3f, 7.6f, 3.6f, theme::COLOR_WEATHER_CLOUD_BACK);
            add_wcloud(badge, theme::COLOR_WEATHER_CLOUD_FRONT);
            break;
        }
        case WC::Rain: {
            add_wcloud(badge, theme::COLOR_WEATHER_CLOUD_LIGHT);
            add_wline(badge, {10, 24, 8, 29}, theme::COLOR_WEATHER_RAIN_DROP, 1.8f);
            add_wline(badge, {16, 24, 14, 29}, theme::COLOR_WEATHER_RAIN_DROP, 1.8f);
            add_wline(badge, {22, 24, 20, 29}, theme::COLOR_WEATHER_RAIN_DROP, 1.8f);
            break;
        }
        case WC::Thunderstorm: {
            add_wcloud(badge, theme::COLOR_WEATHER_CLOUD_STORM);
            // Bolt: unfilled polyline stroke, matching the SVG.
            add_wline(badge, {18.5f, 20, 14, 26.5f, 17, 26.5f, 13.5f, 31.5f},
                      theme::COLOR_WEATHER_BOLT, 2.1f);
            break;
        }
        case WC::Snow: {
            add_wcloud(badge, theme::COLOR_WEATHER_CLOUD_LIGHT);
            const uint32_t snow = theme::COLOR_WEATHER_SNOW;
            // Flake 1 (cx=9)
            add_wline(badge, {9, 23.6f, 9, 28.4f}, snow, 1.3f);
            add_wline(badge, {6.8f, 24.6f, 11.2f, 27.4f}, snow, 1.3f);
            add_wline(badge, {11.2f, 24.6f, 6.8f, 27.4f}, snow, 1.3f);
            // Flake 2 (cx=16)
            add_wline(badge, {16, 24.6f, 16, 29.4f}, snow, 1.3f);
            add_wline(badge, {13.8f, 25.6f, 18.2f, 28.4f}, snow, 1.3f);
            add_wline(badge, {18.2f, 25.6f, 13.8f, 28.4f}, snow, 1.3f);
            // Flake 3 (cx=23)
            add_wline(badge, {23, 23.6f, 23, 28.4f}, snow, 1.3f);
            add_wline(badge, {20.8f, 24.6f, 25.2f, 27.4f}, snow, 1.3f);
            add_wline(badge, {25.2f, 24.6f, 20.8f, 27.4f}, snow, 1.3f);
            break;
        }
        case WC::Fog: {
            const uint32_t fog = theme::COLOR_WEATHER_FOG;
            add_wline(badge, {5, 10, 27, 10}, fog, 2.2f);
            add_wline(badge, {8, 16, 27, 16}, fog, 2.2f);
            add_wline(badge, {5, 22, 24, 22}, fog, 2.2f);
            add_wline(badge, {9, 27, 27, 27}, fog, 2.2f);
            break;
        }
    }
}

// Applies condition/temp_f/visible to one already-built card's badge + bubble.
void apply_weather_to(CardState* s, widget_profile_card::WeatherCondition condition,
                       int temp_f, bool visible) {
    if (!s || !s->weather_badge || !s->temp_bubble || !s->temp_label) return;
    if (!visible) {
        lv_obj_add_flag(s->weather_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->temp_bubble, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clean(s->weather_badge);
    build_weather_icon(s->weather_badge, condition);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d\xC2\xB0", temp_f);  // U+00B0 DEGREE SIGN, UTF-8
    lv_label_set_text(s->temp_label, buf);
    lv_obj_clear_flag(s->weather_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s->temp_bubble, LV_OBJ_FLAG_HIDDEN);
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
    // OVERFLOW_VISIBLE so the presence ring's glow (drawn outside the ring's
    // own box) isn't clipped to the card bounds — LVGL clips a child's draw,
    // including its shadow, to its parent's box by default.
    lv_obj_add_flag(card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* s = new CardState();
    s->photo_img = nullptr;

    // Track the live card so set_photo() can update it without a screen rebuild.
    g_active_card = card;

    // ── Gradient presence ring ────────────────────────────────────────────────
    // A circle 12 px larger than the photo (PHOTO_SIZE + 12 = 6 px each side).
    // Its gradient background IS the "border" — top = presence colour,
    // bottom = darker shade. Replaces the old solid border_color approach so
    // the ring can be updated with a single bg_color / bg_grad_color call.
    // Tap / long-press events are on the inner photo; the ring is non-clickable.
    s->photo_ring = lv_obj_create(card);
    lv_obj_set_size(s->photo_ring, PHOTO_SIZE + 12, PHOTO_SIZE + 12);
    lv_obj_set_style_radius(s->photo_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->photo_ring,
        theme::color(color_for_presence_light(g_default_presence)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s->photo_ring,
        theme::color(color_for_presence_dark(g_default_presence)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(s->photo_ring, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->photo_ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->photo_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->photo_ring, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s->photo_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->photo_ring, LV_OBJ_FLAG_CLICKABLE);

    // Static presence glow — a fixed (non-animated) soft shadow in the
    // presence colour, sitting outside the ring. No breathing/pulse anim,
    // unlike the setup-screen button glow in ui_helpers.cpp.
    lv_obj_set_style_shadow_color(s->photo_ring,
        theme::color(color_for_presence(g_default_presence)), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s->photo_ring, 40, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(s->photo_ring, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s->photo_ring, shadow_opa_for_presence(g_default_presence), LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_x(s->photo_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s->photo_ring, 0, LV_PART_MAIN);
    // The weather badge + temperature bubble (added below) hang partially
    // outside photo_ring's own box. LVGL clips a child's draw to its parent's
    // box by default; OVERFLOW_VISIBLE here relaxes that clip by photo_ring's
    // own ext_draw_size (its shadow above — ~29 px), comfortably covering the
    // badge/bubble's ~8-10 px overhang. Same mechanism as `card`'s flag above,
    // one level down the tree.
    lv_obj_add_flag(s->photo_ring, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // ── Inner photo circle ────────────────────────────────────────────────────
    // The presence ring peeks 6 px around all edges of this circle.
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
    // this card was created (e.g. boot with NVS photo, or screen transition
    // after photo arrived).
    if (g_default_photo) {
        lv_image_set_src(s->photo_img, g_default_photo);
        lv_obj_clear_flag(s->photo_img, LV_OBJ_FLAG_HIDDEN);
    }

    // ── Weather badge ─────────────────────────────────────────────────────────
    // 46 px circle, hangs over the top-left edge of photo_ring. Children of
    // this container are rebuilt from scratch on every set_weather() call
    // (see build_weather_icon()) — cheap, since weather changes at most every
    // 15-30 min (ble-protocol.md §6.3).
    s->weather_badge = lv_obj_create(s->photo_ring);
    lv_obj_set_size(s->weather_badge, 46, 46);
    lv_obj_set_style_radius(s->weather_badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->weather_badge, theme::color(theme::COLOR_ELEV), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->weather_badge, LV_OPA_COVER, LV_PART_MAIN);
    // Thin border — reuses the same elevated-surface border colour as modal
    // cards (ui_helpers.cpp make_modal_layout()) and setup-screen cards.
    lv_obj_set_style_border_color(s->weather_badge, theme::color(theme::COLOR_DIVIDER_STRONG), LV_PART_MAIN);
    lv_obj_set_style_border_width(s->weather_badge, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->weather_badge, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(s->weather_badge, true, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s->weather_badge, theme::color(0x000000), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s->weather_badge, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s->weather_badge, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s->weather_badge, 3, LV_PART_MAIN);
    lv_obj_clear_flag(s->weather_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->weather_badge, LV_OBJ_FLAG_CLICKABLE);
    // Top-left corner of the badge sits 8 px above/left of photo_ring's own
    // top-left corner — mirrors the prototype's `.weather-badge { top:-8px;
    // left:-8px }` relative to `.profile-photo-wrap` (screen-layout.md).
    lv_obj_align(s->weather_badge, LV_ALIGN_TOP_LEFT, -8, -8);
    lv_obj_add_flag(s->weather_badge, LV_OBJ_FLAG_HIDDEN);

    // ── Temperature bubble ────────────────────────────────────────────────────
    // Fixed 60x60 px circle (NOT auto-sized) — hangs over the bottom-right edge
    // of photo_ring. Matches the prototype's `.temp-bubble` fixed-circle change
    // (54px @ 18px font in Ori_UI_Prototype.html; scaled to the firmware's
    // smallest available font, font_body() @ 20px, since there's no 18px asset).
    // 60px comfortably fits the widest string in the valid range, "140\xC2\xB0"
    // (ble-protocol.md §10: temperature_f is -40..140) — measured off this
    // font's glyph advance widths, "140\xC2\xB0" renders ~41px wide against a
    // 60px diameter, leaving ~5-9px clearance at every point of the text's
    // bounding box, comfortably more than "-40\xC2\xB0" (~37px, the other
    // extreme). Container + centered child label mirrors the weather_badge
    // pattern above (fixed shape, content rebuilt/retexted in place).
    s->temp_bubble = lv_obj_create(s->photo_ring);
    lv_obj_set_size(s->temp_bubble, 60, 60);
    lv_obj_set_style_radius(s->temp_bubble, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->temp_bubble, theme::color(theme::COLOR_WEATHER_TEMP_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->temp_bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->temp_bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s->temp_bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s->temp_bubble, theme::color(0x000000), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s->temp_bubble, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s->temp_bubble, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_shadow_ofs_y(s->temp_bubble, 3, LV_PART_MAIN);
    lv_obj_clear_flag(s->temp_bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->temp_bubble, LV_OBJ_FLAG_CLICKABLE);
    // Bottom-right corner of the bubble sits 6 px below / 10 px right of
    // photo_ring's own bottom-right corner — mirrors the prototype's
    // `.temp-bubble { bottom:-6px; right:-10px }` (screen-layout.md).
    lv_obj_align(s->temp_bubble, LV_ALIGN_BOTTOM_RIGHT, 10, 6);
    lv_obj_add_flag(s->temp_bubble, LV_OBJ_FLAG_HIDDEN);

    s->temp_label = lv_label_create(s->temp_bubble);
    lv_label_set_text(s->temp_label, "");
    lv_obj_set_style_text_font(s->temp_label, theme::font_body(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s->temp_label, theme::color(theme::COLOR_WEATHER_TEMP_TEXT), LV_PART_MAIN);
    // Insurance only — at this length ("-40°"/"140°") the label never wraps,
    // but if a future compiled font's glyph widths ever forced a 2-line wrap,
    // this keeps both lines centered rather than left-justified.
    lv_obj_set_style_text_align(s->temp_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_clear_flag(s->temp_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->temp_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s->temp_label);

    // Apply whatever weather was last received (or the hidden default on a
    // freshly-booted device — see g_default_weather_visible).
    apply_weather_to(s, g_default_weather_condition, g_default_weather_temp_f,
                      g_default_weather_visible);

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

lv_obj_t* photo_object(lv_obj_t* card) {
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(card));
    return s ? s->photo_ring : nullptr;
}

void set_presence(lv_obj_t* card, Presence p) {
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(card));
    if (!s || !s->photo_ring) return;
    lv_obj_set_style_bg_color(s->photo_ring,
        theme::color(color_for_presence_light(p)), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(s->photo_ring,
        theme::color(color_for_presence_dark(p)), LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s->photo_ring,
        theme::color(color_for_presence(p)), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s->photo_ring, shadow_opa_for_presence(p), LV_PART_MAIN);
}

void set_default_presence(Presence p) {
    g_default_presence = p;
    if (g_active_card) set_presence(g_active_card, p);
    if (g_modal_photo) {
        lv_obj_set_style_bg_color(g_modal_photo,
            theme::color(color_for_presence_light(p)), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(g_modal_photo,
            theme::color(color_for_presence_dark(p)), LV_PART_MAIN);
        lv_obj_set_style_shadow_color(g_modal_photo,
            theme::color(color_for_presence(p)), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(g_modal_photo, shadow_opa_for_presence(p), LV_PART_MAIN);
    }
    if (g_modal_labels.status_lbl) {
        const char* status_str;
        switch (p) {
            case Presence::Available: status_str = "Available"; break;
            case Presence::Busy:      status_str = "Busy";      break;
            case Presence::Away:      status_str = "Away";      break;
            default:                  status_str = "Offline";   break;
        }
        lv_label_set_text(g_modal_labels.status_lbl, status_str);
        lv_obj_set_style_text_color(g_modal_labels.status_lbl,
            theme::color(color_for_presence(p)), LV_PART_MAIN);
    }
    // Presence dot beside the status word — track colour + glow with presence.
    if (g_modal_labels.status_dot) {
        lv_obj_set_style_bg_color(g_modal_labels.status_dot,
            theme::color(color_for_presence(p)), LV_PART_MAIN);
        lv_obj_set_style_shadow_color(g_modal_labels.status_dot,
            theme::color(color_for_presence(p)), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(g_modal_labels.status_dot,
            shadow_opa_for_presence(p), LV_PART_MAIN);
    }
}

void register_modal_photo(lv_obj_t* photo_obj) { g_modal_photo = photo_obj; }
void unregister_modal_photo()                   { g_modal_photo = nullptr; }

void register_modal_photo_img(lv_obj_t* img_obj) { g_modal_photo_img = img_obj; }
void unregister_modal_photo_img()                { g_modal_photo_img = nullptr; }

void register_modal_labels(const ModalLabels& labels) { g_modal_labels = labels; }
void unregister_modal_labels()                         { g_modal_labels = {}; }

Presence get_default_presence() {
    return g_default_presence;
}

void set_weather(lv_obj_t* card, WeatherCondition condition, int temp_f, bool visible) {
    auto* s = static_cast<CardState*>(lv_obj_get_user_data(card));
    apply_weather_to(s, condition, temp_f, visible);
}

void set_default_weather(WeatherCondition condition, int temp_f, bool visible) {
    g_default_weather_condition = condition;
    g_default_weather_temp_f = temp_f;
    g_default_weather_visible = visible;
    if (g_active_card) set_weather(g_active_card, condition, temp_f, visible);
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

const lv_image_dsc_t* get_photo() {
    return g_default_photo;
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
