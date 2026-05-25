#include "widgets/widget_status_bar.h"

#include "assets/ancs_icons.h"
#include "mock_data.h"
#include "screens/modal_ancs_notification.h"
#include "screens/modal_unpair_phone.h"
#include "screens/screen_repair_phone.h"
#include "state_machine.h"
#include "theme.h"

// Status bar: 800 x 84 px, full panel width, anchored top.
//
// Visual reference: prototype `.status-bar` block.
//   - 22 px horizontal padding
//   - Time (30 px), separator dot, date (22 px) on the left
//   - ANCS icons (48 x 48, gap 14 px) on the right
//   - Phone-disconnect icon (52 x 52) rightmost, only when disconnected
//   - 3 px bottom border in COLOR_DIVIDER
//
// ANCS icons are colored placeholders — the prototype's brand-tile look.
// Real raster/vector assets land in M8 (font_icons or LV_USE_PNG).

namespace {

constexpr int16_t PAD_X         = 22;
constexpr int16_t ICON_SIZE     = 60;
constexpr int16_t ICON_GAP      = 14;
constexpr int16_t PHONE_SIZE    = 64;
constexpr int16_t DATETIME_GAP  = 12;

struct StatusBarState {
    lv_obj_t* datetime_row;
    lv_obj_t* time_label;
    lv_obj_t* sep_label;
    lv_obj_t* date_label;
    lv_obj_t* right_row;
    lv_obj_t* ancs_row;
    lv_obj_t* phone_icon;
    lv_obj_t* mode_toggle;       // 60x60 button at the right edge
    bool      show_datetime;
    bool      pc_connected;      // controls mode_toggle visibility
    bool      phone_bonded;      // true if a phone BLE bond exists
    widget_status_bar::Mode mode;
    widget_status_bar::ModeToggleCb mode_cb;
};

// Solid-colour circle ANCS tile. Brand colours come from ancs_icons::color().
// No text abbreviation — icon identity is purely the colour for now; raster
// assets land in M8.
lv_obj_t* make_ancs_tile(lv_obj_t* parent, const char* token) {
    lv_obj_t* tile = lv_obj_create(parent);
    lv_obj_set_size(tile, ICON_SIZE, ICON_SIZE);
    lv_obj_set_style_bg_color(tile, theme::color(ancs_icons::color(token)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(tile, LV_OPA_60, LV_STATE_PRESSED);

    // Tap → open the ANCS notification detail modal.
    lv_obj_set_user_data(tile, const_cast<char*>(token));
    lv_obj_add_event_cb(tile, [](lv_event_t* e) {
        lv_obj_t* t = lv_event_get_current_target(e);
        const char* tok = static_cast<const char*>(lv_obj_get_user_data(t));
        modal_ancs_notification::create(lv_scr_act(), t, tok);
    }, LV_EVENT_CLICKED, nullptr);

    return tile;
}

// "Phone with diagonal slash" — drawn from primitives to mirror the
// prototype's `#i-phone-broken` SVG. Includes the top mic strip and
// the bottom home-button dot. The diagonal slash is an `lv_line` (NOT
// a rotated rect) — rotating an opaque rect triggers a layer-transparency
// requirement that produces this LVGL warning on every refresh:
//   "lv_draw_sw_layer_create: Rendering this widget needs LV_COLOR_SCREEN_TRANSP 1"
// `lv_line` renders the diagonal as a real line and avoids the warning.
//
// Layout inside the 64 px square (centred to the right_row):
//
//          mic strip  (a short horizontal line near the top of the body)
//         ┌──────────┐
//         │          │
//         │          │
//         │   slash  │   ← diagonal danger-red line crosses the whole box
//         │          │
//         │          │
//         │    ·     │   home-button dot
//         └──────────┘
//
lv_obj_t* make_phone_disconnect(lv_obj_t* parent) {
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, PHONE_SIZE, PHONE_SIZE);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(root, LV_OPA_60, LV_STATE_PRESSED);

    const lv_color_t color = theme::color(theme::COLOR_DANGER);

    // Phone body outline. ~28×46 px centred in the 64 px box.
    lv_obj_t* body = lv_obj_create(root);
    lv_obj_set_size(body, 28, 46);
    lv_obj_center(body);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, color, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(body, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // Top mic / speaker strip — short horizontal line ~3 px below the top
    // of the body. Implemented as a thin rounded rect (no rotation, so no
    // layer-transparency requirement).
    lv_obj_t* mic = lv_obj_create(body);
    lv_obj_set_size(mic, 14, 3);
    lv_obj_align(mic, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_bg_color(mic, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mic, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(mic, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(mic, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mic, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mic, LV_OBJ_FLAG_CLICKABLE);

    // Home-button dot near the bottom of the body.
    lv_obj_t* home = lv_obj_create(body);
    lv_obj_set_size(home, 4, 4);
    lv_obj_align(home, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(home, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(home, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(home, 0, LV_PART_MAIN);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_CLICKABLE);

    // Diagonal slash — bottom-left to top-right of the 64 px box. Drawn as
    // an lv_line so LVGL renders it without needing a transparent layer.
    // The points array must outlive the line widget; static storage is fine
    // since the geometry never changes.
    static lv_point_t slash_pts[] = {
        { 8, PHONE_SIZE - 8 },
        { PHONE_SIZE - 8, 8 },
    };
    lv_obj_t* slash = lv_line_create(root);
    lv_line_set_points(slash, slash_pts, 2);
    lv_obj_set_style_line_color(slash, color, LV_PART_MAIN);
    lv_obj_set_style_line_width(slash, 5, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(slash, true, LV_PART_MAIN);
    lv_obj_clear_flag(slash, LV_OBJ_FLAG_CLICKABLE);

    return root;
}

// Headphones glyph drawn from LVGL primitives — a curved headband + two
// rounded ear cups. Sized for the 60-px mode-toggle button. Mirrors the
// `i-controls` SVG in the HTML prototype.
//
// Band sizing math: the arc bounding box is 28x28. With angles 180..360
// (top half only) the arc's two end-points sit on the box's horizontal
// midline at x = box_center ± 14. We align the box so the midline lands
// at y_btn_center + 1 — exactly where the ear-cup tops are — and so the
// two arc end-points sit directly above the ear-cup centres (also at
// x_btn_center ± 14). That makes the band visually flow into the cups
// instead of floating above them.
lv_obj_t* make_headphones_glyph(lv_obj_t* parent, uint32_t color) {
    // Headband arc — an LVGL Arc primitive set to a top semicircle.
    lv_obj_t* band = lv_arc_create(parent);
    lv_obj_set_size(band, 28, 28);
    lv_obj_align(band, LV_ALIGN_CENTER, 0, +1);
    lv_arc_set_bg_angles(band, 180, 360);     // top half only
    lv_arc_set_value(band, 100);
    lv_obj_remove_style(band, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(band, theme::color(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(band, theme::color(color), LV_PART_MAIN);
    lv_obj_set_style_arc_width(band, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(band, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(band, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(band, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    // Left ear cup.
    lv_obj_t* l_cup = lv_obj_create(parent);
    lv_obj_set_size(l_cup, 7, 11);
    lv_obj_align(l_cup, LV_ALIGN_CENTER, -14, 7);
    lv_obj_set_style_bg_color(l_cup, theme::color(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(l_cup, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(l_cup, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(l_cup, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(l_cup, 0, LV_PART_MAIN);
    lv_obj_clear_flag(l_cup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(l_cup, LV_OBJ_FLAG_SCROLLABLE);

    // Right ear cup.
    lv_obj_t* r_cup = lv_obj_create(parent);
    lv_obj_set_size(r_cup, 7, 11);
    lv_obj_align(r_cup, LV_ALIGN_CENTER, 14, 7);
    lv_obj_set_style_bg_color(r_cup, theme::color(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(r_cup, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(r_cup, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(r_cup, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(r_cup, 0, LV_PART_MAIN);
    lv_obj_clear_flag(r_cup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(r_cup, LV_OBJ_FLAG_SCROLLABLE);

    return band;  // returns one of the children; caller does not need it
}

// Calendar glyph drawn from LVGL primitives — matches the `i-cal` SVG symbol
// used in both the HTML prototype and screen_no_meetings.cpp's make_cal_glyph().
//
// The SVG (viewBox 24×24) has three pieces:
//   1. Hollow rounded body rect  — SVG rect x=3,y=5,w=18,h=16,rx=2
//   2. Thin horizontal divider   — SVG path M3 9 h18  (at y=9 inside the body)
//   3. Two vertical ring tabs    — SVG paths M8 3v4 / M16 3v4
//      (tabs straddle the body top edge: start above y=5, end inside the body)
//
// Scaled to ~2.5× to fill the 60×60 mode-toggle button proportionally.
// Layout (all coords relative to the transparent 60×60 wrapper):
//
//           x=18  x=38
//      y=8  |  |  |  |   ← tabs start (above body top)
//      y=13 └──┴──┴──┘   ← body top edge (tabs straddle this)
//            [  body  ]
//      y=23 ├──────────┤  ← horizontal divider
//            [  body  ]
//      y=53 └──────────┘  ← body bottom
//
// Tab x-centres (SVG x=8 and x=16 at 2.5×): 20 px and 40 px.
// Tab: 4 px wide, 10 px tall, top at y=8 (5 px above body top at y=13),
//      bottom at y=18 (5 px into the body — just below the divider line).
lv_obj_t* make_calendar_glyph(lv_obj_t* parent, uint32_t color) {
    // Transparent 60×60 positioning wrapper — same pattern as make_cal_glyph()
    // in screen_no_meetings.cpp. Children use lv_obj_set_pos() for exact pixel
    // placement without relying on LV_ALIGN_CENTER offset arithmetic.
    lv_obj_t* glyph = lv_obj_create(parent);
    lv_obj_set_size(glyph, 60, 60);
    lv_obj_align(glyph, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(glyph, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(glyph, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(glyph, 0, LV_PART_MAIN);
    lv_obj_clear_flag(glyph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(glyph, LV_OBJ_FLAG_CLICKABLE);

    const lv_color_t c = theme::color(color);

    // 1) Body — hollow rounded rect. 45×40 px at pos (8, 13), radius 5.
    //    SVG: x=3,y=5,w=18,h=16,rx=2  →  ×2.5: w=45,h=40, rx=5.
    //    Horizontally centred in 60 px: x = (60-45)/2 = 7 → rounded to 8.
    lv_obj_t* body = lv_obj_create(glyph);
    lv_obj_set_size(body, 45, 40);
    lv_obj_set_pos(body, 8, 13);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, c, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(body, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 2) Horizontal divider — full body width, 3 px tall.
    //    SVG: M3 9 h18 (y=9 → 4 units below body top at y=5 → 4×2.5=10 px into body).
    //    Absolute y = body_top + 10 = 13 + 10 = 23.
    lv_obj_t* divider = lv_obj_create(glyph);
    lv_obj_set_size(divider, 45, 3);
    lv_obj_set_pos(divider, 8, 23);
    lv_obj_set_style_bg_color(divider, c, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(divider, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    // 3) Two vertical ring tabs straddling the body's top edge.
    //    SVG: M8 3v4 and M16 3v4  (y=3..7, width = stroke-width 1.6 → 4 px at 2.5×).
    //    Tab x-centres at SVG x=8 and x=16 → 8×2.5=20 and 16×2.5=40 absolute.
    //    Tab top: SVG y=3 → 3×2.5=7.5 → y=8. Tab height: 4×2.5=10 px.
    auto make_tab = [&](int16_t cx_abs) {
        lv_obj_t* tab = lv_obj_create(glyph);
        lv_obj_set_size(tab, 4, 10);
        lv_obj_set_pos(tab, cx_abs - 2, 8);
        lv_obj_set_style_bg_color(tab, c, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(tab, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(tab, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_CLICKABLE);
    };
    make_tab(20);   // left ring tab — SVG x=8 × 2.5
    make_tab(40);   // right ring tab — SVG x=16 × 2.5

    return glyph;
}

constexpr int16_t MODE_TOGGLE_SIZE = 60;

// Re-draw the mode-toggle visuals to reflect Calendar vs Controls (Keyboard).
// Icon shows the mode you will SWITCH TO — so the glyph is the OPPOSITE of
// the current mode:
//   In Calendar mode  → headphones glyph, neutral bg   ("tap to enter Controls")
//   In Controls mode  → calendar glyph, accent-tinted bg ("tap to return to Calendar")
void rebuild_mode_toggle_glyph(lv_obj_t* btn, widget_status_bar::Mode mode) {
    lv_obj_clean(btn);
    if (mode == widget_status_bar::Mode::Calendar) {
        lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_ELEV), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_DIVIDER), LV_PART_MAIN);
        make_headphones_glyph(btn, theme::COLOR_TEXT_PRIMARY);
    } else {
        // Controls (Keyboard) mode active — show calendar icon to return.
        lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_ACCENT_SOFT), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_ACCENT_LINE), LV_PART_MAIN);
        make_calendar_glyph(btn, theme::COLOR_ACCENT);
    }
}

void on_mode_toggle_tap(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    auto* state = static_cast<StatusBarState*>(lv_event_get_user_data(e));
    if (state && state->mode_cb) state->mode_cb();
}

lv_obj_t* make_mode_toggle(lv_obj_t* parent, StatusBarState* state) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_size(btn, MODE_TOGGLE_SIZE, MODE_TOGGLE_SIZE);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, on_mode_toggle_tap, LV_EVENT_CLICKED, state);
    rebuild_mode_toggle_glyph(btn, state->mode);
    return btn;
}

} // namespace

namespace widget_status_bar {

// Module-level defaults — set via set_default_* helpers. Newly-created status
// bars read these so screen_manager can change state once per screen-switch.
bool g_default_pc_connected   = true;
bool g_default_phone_bonded   = false;
Mode g_default_mode           = Mode::Calendar;
ModeToggleCb g_default_mode_cb = nullptr;

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_obj_get_width(parent), HEIGHT);
    lv_obj_set_pos(bar, 0, 0);

    lv_obj_set_style_bg_color(bar, theme::color(theme::COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bar, PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_right(bar, PAD_X, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, theme::color(theme::COLOR_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 3, LV_PART_MAIN);  // 3 px per screen-layout.md
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* state = new StatusBarState();
    state->show_datetime = true;
    state->pc_connected  = g_default_pc_connected;
    state->phone_bonded  = g_default_phone_bonded;
    state->mode          = g_default_mode;
    state->mode_cb       = g_default_mode_cb;
    state->mode_toggle   = nullptr;

    // ===== Date/time block =====
    state->datetime_row = lv_obj_create(bar);
    lv_obj_set_size(state->datetime_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(state->datetime_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->datetime_row, 0, 0);
    lv_obj_set_style_pad_all(state->datetime_row, 0, 0);
    lv_obj_clear_flag(state->datetime_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(state->datetime_row, LV_FLEX_FLOW_ROW);
    // Cross-axis CENTER so the 30 px time and 22 px sep/date are vertically
    // centered to each other; the row as a whole is then centered in the
    // 84 px bar by the parent's LV_FLEX_ALIGN_CENTER.
    lv_obj_set_flex_align(state->datetime_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state->datetime_row, DATETIME_GAP, 0);

    state->time_label = lv_label_create(state->datetime_row);
    lv_obj_set_style_text_color(state->time_label, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(state->time_label, theme::font_time(), 0);

    state->sep_label = lv_label_create(state->datetime_row);
    lv_label_set_text_static(state->sep_label, "\xC2\xB7");  // U+00B7 MIDDLE DOT, static literal
    lv_obj_set_style_text_color(state->sep_label, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(state->sep_label, theme::font_meta(), 0);

    state->date_label = lv_label_create(state->datetime_row);
    lv_obj_set_style_text_color(state->date_label, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(state->date_label, theme::font_meta(), 0);

    // ===== Spacer (flex-grow) =====
    lv_obj_t* spacer = lv_obj_create(bar);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    // ===== Right cluster: ANCS row, phone-disconnect, mode-toggle =====
    //
    // These three sit directly in the bar's flex (not nested in a
    // `right_row` container) so the bar's flex algorithm sees all of
    // them up-front and can size + position them correctly even as
    // visibility toggles at runtime. An earlier nested-container design
    // relied on LV_SIZE_CONTENT to re-sum children when visibility
    // changed; LVGL's flex did not propagate that reliably and the
    // phone-disconnect glyph ended up stacked on top of the mode-toggle
    // in the same flex cell. Putting them in the bar directly lets the
    // bar's flex use the spacer's flex_grow to absorb the slack.
    //
    // `right_row` is kept as an alias for `ancs_row` because some
    // existing code references it (and `ancs_row` is the only sub-
    // container that needs its own flex — for the variable number of
    // ANCS tiles with the right inter-tile gap).
    state->ancs_row = lv_obj_create(bar);
    state->right_row = state->ancs_row;  // alias for legacy references
    // Width is recomputed by refresh() based on the actual tile count
    // (LV_SIZE_CONTENT does NOT re-expand reliably after lv_obj_clean()).
    // Start at 0 — refresh() runs at the end of create() and sets the
    // right value before the bar is laid out.
    lv_obj_set_size(state->ancs_row, 0, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(state->ancs_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->ancs_row, 0, 0);
    lv_obj_set_style_pad_all(state->ancs_row, 0, 0);
    lv_obj_clear_flag(state->ancs_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(state->ancs_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state->ancs_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state->ancs_row, ICON_GAP, 0);

    // Phone-disconnect glyph — direct child of bar, between ANCS and
    // mode-toggle. Initially hidden; shown by set_phone_connected(false).
    state->phone_icon = make_phone_disconnect(bar);
    lv_obj_add_flag(state->phone_icon, LV_OBJ_FLAG_HIDDEN);

    // Long-press (3 s) on the phone-disconnect icon opens
    // either the re-pair phone screen (no existing bond) or the unpair
    // phone modal (bond exists).  LV_EVENT_LONG_PRESSED fires after the
    // indev long_press_time set in main.cpp (3000 ms).
    lv_obj_add_event_cb(state->phone_icon, [](lv_event_t* e) {
        auto* st = static_cast<StatusBarState*>(lv_event_get_user_data(e));
        if (!st) return;
        lv_obj_t* screen = lv_scr_act();
        if (st->phone_bonded) {
            // Phone already bonded — offer to unpair.
            modal_unpair_phone::create(screen);
        } else {
            // No bond — push re-pair phone screen.
            // The re-pair screen's Cancel button calls state_machine::evaluate()
            // to restore the correct runtime screen.
            lv_obj_t* repair = screen_repair_phone::create();
            lv_scr_load(repair);
            lv_refr_now(lv_disp_get_default());
            lv_obj_del(screen);
        }
    }, LV_EVENT_LONG_PRESSED, state);

    // Mode-toggle — direct child of bar, rightmost when visible.
    state->mode_toggle = make_mode_toggle(bar, state);
    if (!state->pc_connected) {
        lv_obj_add_flag(state->mode_toggle, LV_OBJ_FLAG_HIDDEN);
    }
    // 16 px column gap between bar children (datetime, spacer, ancs,
    // phone, mode). Spacer takes flex_grow=1 → absorbs slack so the
    // right cluster stays anchored to the right edge.
    lv_obj_set_style_pad_column(bar, 16, 0);

    lv_obj_set_user_data(bar, state);
    lv_obj_add_event_cb(bar, [](lv_event_t* e) {
        delete static_cast<StatusBarState*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, state);
    refresh(bar);
    return bar;
}

void set_show_datetime(lv_obj_t* bar, bool show) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->show_datetime = show;
    if (show) lv_obj_clear_flag(s->datetime_row, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(s->datetime_row, LV_OBJ_FLAG_HIDDEN);
}

void set_phone_connected(lv_obj_t* bar, bool connected) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;

    if (connected) {
        lv_obj_add_flag(s->phone_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s->ancs_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s->ancs_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s->phone_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_pc_connected(lv_obj_t* bar, bool connected) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->pc_connected = connected;
    if (!s->mode_toggle) return;
    if (connected) lv_obj_clear_flag(s->mode_toggle, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(s->mode_toggle,  LV_OBJ_FLAG_HIDDEN);
}

void set_mode(lv_obj_t* bar, Mode mode) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->mode = mode;
    if (s->mode_toggle) rebuild_mode_toggle_glyph(s->mode_toggle, mode);
}

void set_mode_toggle_cb(lv_obj_t* bar, ModeToggleCb cb) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->mode_cb = cb;
}

void set_phone_bonded(lv_obj_t* bar, bool bonded) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->phone_bonded = bonded;
}

void set_default_pc_connected(bool connected)     { g_default_pc_connected = connected; }
void set_default_phone_bonded(bool bonded)         { g_default_phone_bonded = bonded; }
void set_default_mode(Mode mode)                   { g_default_mode = mode; }
void set_default_mode_toggle_cb(ModeToggleCb cb)   { g_default_mode_cb = cb; }

void refresh(lv_obj_t* bar) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;

    const auto& t = mock_data::now();
    lv_label_set_text(s->time_label, t.hh_mm);
    lv_label_set_text(s->date_label, t.date_short);

    // Rebuild ANCS row from current config.
    const auto& cfg = mock_data::ancs_config();
    lv_obj_clean(s->ancs_row);
    size_t visible_tiles = 0;
    if (cfg.phone_connected) {
        for (size_t i = 0; i < cfg.count; ++i) {
            if (cfg.icons[i]) {
                make_ancs_tile(s->ancs_row, cfg.icons[i]);
                ++visible_tiles;
            }
        }
    }
    // Size the ANCS row to exactly its tile count so the bar's flex
    // doesn't overshoot and push the mode-toggle off the right edge.
    // (LV_SIZE_CONTENT does not re-expand the row after lv_obj_clean()
    // in LVGL 8, so we compute the width explicitly.)
    int ancs_w = (visible_tiles == 0)
                 ? 0
                 : (int)visible_tiles * ICON_SIZE + ((int)visible_tiles - 1) * ICON_GAP;
    lv_obj_set_width(s->ancs_row, ancs_w);

    set_phone_connected(bar, cfg.phone_connected);
}

} // namespace widget_status_bar
