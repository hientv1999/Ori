#include "screens/screen_clock.h"

#include <lvgl.h>
#include <stdio.h>
#include <time.h>

#include "app_state.h"
#include "theme.h"
#include "time_format.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Digital clock screen — left panel only, entered by tapping the status-bar
// time. Status bar is present but its date/time is hidden because the clock
// face IS the time. Mode-toggle acts as a return button (calendar glyph).
//
// The colon breathes at 0.5 Hz (2 s/cycle) via a simple timer-driven fade
// that toggles between full and 25% opacity.

namespace {

// Colon breathes at 0.5 Hz — one full fade-out+fade-in cycle every 2000 ms
// (20 fps, 50 ms/tick × 40 ticks = 2000 ms exactly).
// Ticks 0-19: opacity 255→64 (fade out). Ticks 20-39: opacity 64→255 (fade in).
constexpr uint32_t BLINK_INTERVAL_MS  = 50;
constexpr uint16_t BLINK_HALF_TICKS   = 20;
constexpr uint16_t BLINK_TOTAL_TICKS  = 40;

struct ClockFaceState {
    lv_obj_t*  hour_lbl;
    lv_obj_t*  minute_lbl;
    lv_obj_t*  ampm_lbl;   // "AM"/"PM" subtext, pinned beside minute_lbl — see update_clock_labels()
    lv_obj_t*  date_lbl;
    lv_timer_t* update_timer;
    // Last-rendered state — the display only actually changes once a minute
    // (hour/minute/ampm) or once a day (date), but this timer ticks every
    // second. int16_t sentinels of -1 (never a valid tm_min/tm_mday) force a
    // render on the very first call regardless of the other cached fields.
    int16_t    last_min  = -1;
    int16_t    last_mday = -1;
    bool       last_h24  = true;
    bool       last_clock_set = false;
};

static void update_clock_labels(ClockFaceState* cf) {
    // No battery-backed RTC: until Orion's first Time Sync, the clock is unknown.
    // Show "--:--" + a hint rather than a fabricated ~1970 time/date.
    bool clock_set = app_state::clock_is_set();
    if (!clock_set) {
        if (cf->last_clock_set || cf->last_min != -1) {
            // Only re-render the "waiting" state once, on the transition into
            // it — it never changes again until clock_set flips back to true.
            lv_label_set_text(cf->hour_lbl,   "--");
            lv_label_set_text(cf->minute_lbl, "--");
            lv_label_set_text(cf->date_lbl,   "WAITING FOR ORION");
            lv_obj_add_flag(cf->ampm_lbl, LV_OBJ_FLAG_HIDDEN);
            cf->last_clock_set = false;
            cf->last_min  = -1;
            cf->last_mday = -1;
        }
        return;
    }
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);

    bool h24 = time_format::is_24h();
    bool unchanged = cf->last_clock_set && cf->last_min == tm.tm_min &&
                     cf->last_mday == tm.tm_mday && cf->last_h24 == h24;
    if (unchanged) return;
    cf->last_clock_set = true;
    cf->last_min  = (int16_t)tm.tm_min;
    cf->last_mday = (int16_t)tm.tm_mday;
    cf->last_h24  = h24;

    char buf[8];
    // The XL clock font has digits + ':' + '-' only (no letters), so AM/PM can't
    // ride inside hour_lbl/minute_lbl itself — ampm_lbl is a separate small label
    // in its own right_slot instead (see create()), which doesn't affect
    // hour/minute's centering. 12-hour drops the leading zero on the hour.
    if (h24) {
        snprintf(buf, sizeof(buf), "%02d", tm.tm_hour);
    } else {
        int h12 = tm.tm_hour % 12;
        if (h12 == 0) h12 = 12;
        snprintf(buf, sizeof(buf), "%d", h12);
    }
    lv_label_set_text(cf->hour_lbl, buf);
    snprintf(buf, sizeof(buf), "%02d", tm.tm_min);
    lv_label_set_text(cf->minute_lbl, buf);
    char day[16], mon[8];
    strftime(day, sizeof(day), "%A", &tm);
    strftime(mon, sizeof(mon), "%B",  &tm);
    char date_buf[56];
    // Uppercase: ESP32 strftime may not support %^A/%^B, so do it manually.
    ui::uppercase_ascii(day);
    ui::uppercase_ascii(mon);
    // No AM/PM suffix here anymore (it moved to ampm_lbl next to the time) —
    // the date strip is now the same width/format in both 12h and 24h, so it
    // stays centered under the clock in either mode.
    snprintf(date_buf, sizeof(date_buf), "%s, %s %d, %d", day, mon, tm.tm_mday, tm.tm_year + 1900);
    lv_label_set_text(cf->date_lbl, date_buf);

    if (h24) {
        lv_obj_add_flag(cf->ampm_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(cf->ampm_lbl, tm.tm_hour < 12 ? "AM" : "PM");
        lv_obj_clear_flag(cf->ampm_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

struct ColonState {
    lv_obj_t*   colon;
    uint16_t    tick;
    lv_timer_t* timer;
};

void colon_blink_timer_cb(lv_timer_t* t) {
    auto* cs = static_cast<ColonState*>(lv_timer_get_user_data(t));
    cs->tick = (uint16_t)((cs->tick + 1) % BLINK_TOTAL_TICKS);
    lv_opa_t opa;
    if (cs->tick < BLINK_HALF_TICKS) {
        opa = (lv_opa_t)(255 - (191 * cs->tick) / BLINK_HALF_TICKS);
    } else {
        uint16_t t2 = cs->tick - BLINK_HALF_TICKS;
        opa = (lv_opa_t)(64 + (191 * t2) / BLINK_HALF_TICKS);
    }
    lv_obj_set_style_opa(cs->colon, opa, 0);
}

} // namespace

namespace screen_clock {

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    lv_obj_t* bar = widget_status_bar::create(screen);
    widget_status_bar::set_show_datetime(bar, false);

    lv_obj_t* body = ui::make_screen_body(screen);

    // Left panel — clock face.
    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, 528, lv_pct(100));
    ui::clear_container(left);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Row holding [left_spacer][center_wrap: hour+colon+minute][right_slot: AM/PM].
    // left_spacer and right_slot both have flex_grow=1 and are ALWAYS present
    // (only ampm inside right_slot toggles hidden/visible) — two equal-growing
    // tracks around a fixed-content middle is the standard trick for "keep this
    // centered regardless of what's beside it": since both growers always
    // exist and always grow equally, center_wrap's horizontal position never
    // depends on whether AM/PM has content. time_row needs an explicit width
    // (not LV_SIZE_CONTENT) for "grow" to have any leftover space to split.
    lv_obj_t* time_row = lv_obj_create(left);
    lv_obj_set_size(time_row, lv_pct(100), LV_SIZE_CONTENT);
    ui::clear_container(time_row);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* left_spacer = lv_obj_create(time_row);
    lv_obj_set_size(left_spacer, 1, 1);
    ui::clear_container(left_spacer);
    lv_obj_set_flex_grow(left_spacer, 1);

    // center_wrap — hour, colon, minute. This is what stays centered.
    lv_obj_t* center_wrap = lv_obj_create(time_row);
    lv_obj_set_size(center_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ui::clear_container(center_wrap);
    lv_obj_set_style_pad_column(center_wrap, 12, 0);
    lv_obj_set_flex_flow(center_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center_wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* hour = lv_label_create(center_wrap);
    lv_obj_set_style_text_color(hour, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(hour, theme::font_clock_xl(), 0);
    lv_obj_set_style_text_letter_space(hour, -3, 0);

    // Colon — two circular dots instead of font_clock_xl's ':' glyph (which
    // renders as small squares at this size). colon_wrap is sized to match
    // the glyph's own advance width (23px, per ori_font_hanken_96.c) so
    // hour/minute spacing is unchanged from before. Blink targets colon_wrap
    // itself — LVGL fades a whole subtree together when opacity is set on a
    // container, same as it did on the single label before.
    constexpr int16_t COLON_DOT_SIZE = 16;
    constexpr int16_t COLON_GAP      = 14;
    constexpr int16_t COLON_WRAP_W   = 23;
    lv_obj_t* colon = lv_obj_create(center_wrap);
    lv_obj_set_size(colon, COLON_WRAP_W, COLON_DOT_SIZE * 2 + COLON_GAP);
    ui::clear_container(colon);
    auto make_colon_dot = [&](lv_align_t align) {
        lv_obj_t* dot = lv_obj_create(colon);
        lv_obj_set_size(dot, COLON_DOT_SIZE, COLON_DOT_SIZE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, theme::color(theme::COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dot, align, 0, 0);
    };
    make_colon_dot(LV_ALIGN_TOP_MID);
    make_colon_dot(LV_ALIGN_BOTTOM_MID);

    lv_obj_t* minute = lv_label_create(center_wrap);
    lv_obj_set_style_text_color(minute, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(minute, theme::font_clock_xl(), 0);
    lv_obj_set_style_text_letter_space(minute, -3, 0);

    // right_slot — always present (grow=1, matching left_spacer) so
    // center_wrap's centering never depends on AM/PM. ampm is a plain,
    // non-flex child positioned once via lv_obj_align (a persistent style,
    // not a one-shot calc — LVGL keeps it live automatically as right_slot's
    // own size changes). Hidden in 24-hour format (update_clock_labels()
    // toggles this every second).
    lv_obj_t* right_slot = lv_obj_create(time_row);
    lv_obj_set_size(right_slot, 1, LV_SIZE_CONTENT);
    ui::clear_container(right_slot);
    lv_obj_set_flex_grow(right_slot, 1);

    lv_obj_t* ampm = lv_label_create(right_slot);
    lv_obj_set_style_text_color(ampm, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(ampm, theme::font_h2(), 0);  // 28px
    lv_obj_align(ampm, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(ampm, LV_OBJ_FLAG_HIDDEN); // shown by the first update_clock_labels() call if 12h

    // Date strip below — "WEDNESDAY, MAY 14, 2026"
    lv_obj_t* date = lv_label_create(left);
    lv_obj_set_style_text_color(date, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(date, theme::font_time(), 0);
    lv_obj_set_style_pad_top(date, 22, 0);

    // Populate labels immediately with real time, then keep them live via a
    // 1-second timer. The colon blink runs independently at BLINK_INTERVAL_MS
    // (50 ms / 20 fps) cadence.
    auto* cf = new ClockFaceState{hour, minute, ampm, date, nullptr};
    update_clock_labels(cf);
    cf->update_timer = lv_timer_create([](lv_timer_t* t) {
        update_clock_labels(static_cast<ClockFaceState*>(lv_timer_get_user_data(t)));
    }, 1000, cf);

    // Colon breathing — 20 fps timer, opacity 255 ↔ 64 over 2000 ms (0.5 Hz).
    auto* cs = new ColonState{colon, 0, nullptr};
    cs->timer = lv_timer_create(colon_blink_timer_cb, BLINK_INTERVAL_MS, cs);

    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto* cf = static_cast<ClockFaceState*>(lv_event_get_user_data(e));
        if (cf) { lv_timer_delete(cf->update_timer); delete cf; }
    }, LV_EVENT_DELETE, cf);
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto* cs = static_cast<ColonState*>(lv_event_get_user_data(e));
        if (cs) { lv_timer_delete(cs->timer); delete cs; }
    }, LV_EVENT_DELETE, cs);

    ui::make_panel_divider(body);
    widget_profile_card::create(body);
    return screen;
}

} // namespace screen_clock
