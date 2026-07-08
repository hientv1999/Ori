#include "screens/screen_clock_analog.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "app_state.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Analog clock screen — alternate face for the Clock state, entered the same
// way as the digital face (screen_clock.cpp): tapping the status-bar time.
// Which face is shown is a user preference (state_machine.cpp `g_clock_face`,
// persisted via nvs::get/set_clock_face()) — intended to eventually be set
// from Orion; for now also reachable via the ORI_DEBUG_SERIAL cycler ('a' /
// 'c' keys) for hardware testing.
//
// Dial geometry mirrors the approved prototype face (Ori_UI_Prototype.js
// analogClockHTML()): 280 px dial, minimal tick-only face (no outer ring),
// hour/minute hands in primary text color, accent-colored second hand.

namespace {

constexpr int   kDialSize = 280;
constexpr float kCenter   = kDialSize / 2.0f;   // 140
constexpr float kPi       = 3.14159265358979323846f;

// Tick endpoints are static for the life of the screen (the face never
// redraws), mirroring the static lv_point_precise_t pattern already used for
// the setup/OTA spinner checkmarks (lv_line stores a pointer, not a copy, so
// the backing array must outlive the line object).
lv_point_precise_t g_tick_pts[12][2];

struct AnalogFaceState {
    lv_obj_t*  hour_hand;
    lv_obj_t*  minute_hand;
    lv_obj_t*  second_hand;
    lv_obj_t*  date_lbl;
    lv_timer_t* update_timer;
    lv_point_precise_t hour_pts[2];
    lv_point_precise_t minute_pts[2];
    lv_point_precise_t second_pts[2];
    // date_lbl only changes once a day — unlike the hands, which must redraw
    // every tick, cache it to skip the redundant lv_label_set_text() on the
    // ~86399/86400 ticks where it's unchanged.
    int16_t    last_mday = -1;
    bool       last_clock_set = false;
};

// angle_deg: 0 = 12 o'clock, increases clockwise — matches a normal clock
// face and the clockwise-sweep convention used by every other progress ring
// in this codebase (hardware.md LVGL rendering rules).
void point_at(float angle_deg, float radius, lv_point_precise_t* out) {
    float rad = (angle_deg - 90.0f) * kPi / 180.0f;
    out->x = (lv_value_precise_t)lroundf(kCenter + radius * cosf(rad));
    out->y = (lv_value_precise_t)lroundf(kCenter + radius * sinf(rad));
}

void update_hand(lv_obj_t* line, lv_point_precise_t* pts, float angle_deg, float length) {
    pts[0].x = (lv_value_precise_t)lroundf(kCenter);
    pts[0].y = (lv_value_precise_t)lroundf(kCenter);
    point_at(angle_deg, length, &pts[1]);
    lv_line_set_points(line, pts, 2);
}

void update_face(AnalogFaceState* af) {
    // No battery-backed RTC: until Orion's first Time Sync, the clock is
    // unknown. Park the hands at 12 rather than showing a fabricated time —
    // same rule screen_clock.cpp applies to its "--:--" digital readout.
    if (!app_state::clock_is_set()) {
        if (af->last_clock_set || af->last_mday != -1) {
            // Only re-render/re-park once, on the transition into this state
            // — nothing here changes again until clock_set flips back true.
            lv_label_set_text(af->date_lbl, "WAITING FOR ORION");
            update_hand(af->hour_hand,   af->hour_pts,   0.0f, 68.0f);
            update_hand(af->minute_hand, af->minute_pts, 0.0f, 102.0f);
            update_hand(af->second_hand, af->second_pts, 0.0f, 112.0f);
            af->last_clock_set = false;
            af->last_mday = -1;
        }
        return;
    }

    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);

    // Hands move every second — always redraw these, unlike date_lbl below.
    float hour_deg   = (tm.tm_hour % 12) * 30.0f + tm.tm_min * 0.5f;
    float minute_deg = tm.tm_min * 6.0f + tm.tm_sec * 0.1f;
    float second_deg = tm.tm_sec * 6.0f;
    update_hand(af->hour_hand,   af->hour_pts,   hour_deg,   68.0f);
    update_hand(af->minute_hand, af->minute_pts, minute_deg, 102.0f);
    update_hand(af->second_hand, af->second_pts, second_deg, 112.0f);

    bool date_unchanged = af->last_clock_set && af->last_mday == tm.tm_mday;
    af->last_clock_set = true;
    af->last_mday = (int16_t)tm.tm_mday;
    if (date_unchanged) return;

    char day[16], mon[8];
    strftime(day, sizeof(day), "%A", &tm);
    strftime(mon, sizeof(mon), "%B",  &tm);
    // Uppercase: ESP32 strftime may not support %^A/%^B, so do it manually.
    for (char* p = day; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;
    for (char* p = mon; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;
    // No AM/PM suffix — the analog dial has no digital time readout to pair it
    // with, so it just reads as a plain date. Year included (unlike the
    // digital face's date strip, which shares the same "%s, %s %d, %d" shape).
    char date_buf[56];
    snprintf(date_buf, sizeof(date_buf), "%s, %s %d, %d", day, mon, tm.tm_mday, tm.tm_year + 1900);
    lv_label_set_text(af->date_lbl, date_buf);
}

void make_tick(lv_obj_t* dial, int index) {
    bool  major = (index % 3) == 0;  // 12, 3, 6, 9
    float deg   = index * 30.0f;
    float outer = 130.0f;
    float inner = major ? 110.0f : 119.0f;

    point_at(deg, outer, &g_tick_pts[index][0]);
    point_at(deg, inner, &g_tick_pts[index][1]);

    lv_obj_t* tick = lv_line_create(dial);
    lv_line_set_points(tick, g_tick_pts[index], 2);
    lv_obj_set_style_line_color(tick,
        theme::color(major ? theme::COLOR_TEXT_SECONDARY : theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_line_width(tick, major ? 4 : 2, 0);
    lv_obj_set_style_line_rounded(tick, true, 0);
}

} // namespace

namespace screen_clock_analog {

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    lv_obj_t* bar = widget_status_bar::create(screen);
    widget_status_bar::set_show_datetime(bar, false);

    lv_obj_t* body = ui::make_screen_body(screen);

    // Left panel — analog dial centred, date strip below.
    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, 528, lv_pct(100));
    ui::clear_container(left);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Dial — transparent housing for the ticks + hands. No outer ring; the
    // ticks alone form the face (matches the approved prototype design).
    lv_obj_t* dial = lv_obj_create(left);
    lv_obj_set_size(dial, kDialSize, kDialSize);
    ui::clear_container(dial);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 12; ++i) make_tick(dial, i);

    auto* af = new AnalogFaceState{};

    af->hour_hand = lv_line_create(dial);
    lv_obj_set_style_line_color(af->hour_hand, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_line_width(af->hour_hand, 7, 0);
    lv_obj_set_style_line_rounded(af->hour_hand, true, 0);

    af->minute_hand = lv_line_create(dial);
    lv_obj_set_style_line_color(af->minute_hand, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_line_width(af->minute_hand, 5, 0);
    lv_obj_set_style_line_rounded(af->minute_hand, true, 0);

    af->second_hand = lv_line_create(dial);
    lv_obj_set_style_line_color(af->second_hand, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_line_width(af->second_hand, 2, 0);
    lv_obj_set_style_line_rounded(af->second_hand, true, 0);

    // Center hub.
    lv_obj_t* hub = lv_obj_create(dial);
    lv_obj_set_size(hub, 12, 12);
    lv_obj_set_pos(hub, (int)kCenter - 6, (int)kCenter - 6);
    ui::clear_container(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_CLICKABLE);

    // Date strip below — "WEDNESDAY, MAY 14, 2026". No AM/PM (see update_face()) —
    // the dial has no digital time readout to pair a suffix with.
    lv_obj_t* date = lv_label_create(left);
    lv_obj_set_style_text_color(date, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(date, theme::font_time(), 0);
    lv_obj_set_style_pad_top(date, 26, 0);
    af->date_lbl = date;

    update_face(af);
    af->update_timer = lv_timer_create([](lv_timer_t* t) {
        update_face(static_cast<AnalogFaceState*>(lv_timer_get_user_data(t)));
    }, 1000, af);

    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto* af = static_cast<AnalogFaceState*>(lv_event_get_user_data(e));
        if (af) { lv_timer_delete(af->update_timer); delete af; }
    }, LV_EVENT_DELETE, af);

    ui::make_panel_divider(body);
    widget_profile_card::create(body);
    return screen;
}

} // namespace screen_clock_analog
