#include "screens/screen_clock.h"

#include <lvgl.h>

#include "mock_data.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Digital clock screen — left panel only, entered by tapping the status-bar
// time. Status bar is present but its date/time is hidden because the clock
// face IS the time. Mode-toggle acts as a return button (calendar glyph).
//
// TODO(M8): large-digit clock font — see CLAUDE.md M8 milestone for details.
//
// The colon blinks at the prototype's 1.5 s cadence via a simple LVGL
// animation that toggles between full and 25% opacity.

namespace {

// Colon blink at 24 fps (42 ms/tick). One cycle = 36 ticks ≈ 1512 ms.
// Ticks 0-17: opacity 255→64 (fade out). Ticks 18-35: opacity 64→255 (fade in).
constexpr uint32_t BLINK_INTERVAL_MS  = 42;
constexpr uint16_t BLINK_HALF_TICKS   = 18;
constexpr uint16_t BLINK_TOTAL_TICKS  = 36;

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

    // Row holding hour, colon, minute (so we can blink only the colon).
    lv_obj_t* time_row = lv_obj_create(left);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ui::clear_container(time_row);
    lv_obj_set_style_pad_column(time_row, 12, 0);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const auto& t = mock_data::now();

    lv_obj_t* hour = lv_label_create(time_row);
    lv_label_set_text(hour, t.hour_12);
    lv_obj_set_style_text_color(hour, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(hour, theme::font_large(), 0);

    lv_obj_t* colon = lv_label_create(time_row);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_color(colon, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(colon, theme::font_large(), 0);

    lv_obj_t* minute = lv_label_create(time_row);
    lv_label_set_text(minute, t.minute);
    lv_obj_set_style_text_color(minute, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(minute, theme::font_large(), 0);

    // Date strip below — "WEDNESDAY, MAY 14 · PM"
    char date_buf[64];
    lv_snprintf(date_buf, sizeof(date_buf), "%s · %s", t.date_long, t.ampm);
    lv_obj_t* date = lv_label_create(left);
    lv_label_set_text(date, date_buf);
    lv_obj_set_style_text_color(date, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    // font_time() (30 px) matches the status-bar time and the profile name —
    // the three "primary identity" type sizes across the device.
    lv_obj_set_style_text_font(date, theme::font_time(), 0);
    lv_obj_set_style_pad_top(date, 18, 0);

    // Colon blink — 24 fps timer, opacity 255 ↔ 64 over ~1512 ms.
    auto* cs = new ColonState{colon, 0, nullptr};
    cs->timer = lv_timer_create(colon_blink_timer_cb, BLINK_INTERVAL_MS, cs);

    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto* cs = static_cast<ColonState*>(lv_event_get_user_data(e));
        if (cs) { lv_timer_delete(cs->timer); delete cs; }
    }, LV_EVENT_DELETE, cs);

    ui::make_panel_divider(body);
    widget_profile_card::create(body);
    return screen;
}

} // namespace screen_clock
