#include "screens/modal_countdown.h"

#include <cstdio>
#include <lvgl.h>

#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_progress_ring.h"

// 5-minute pre-meeting countdown modal.
//
// Visual reference: prototype `.modal-scrim` + `.countdown` block. A blurred
// scrim isn't representable on LVGL 8 RGB565 without a custom shader, so we
// fall back to a heavy opacity dimming (LV_OPA_80 of COLOR_SCRIM) — visually
// close to the prototype's backdrop-filter at the read distance of a desk
// display. The ring + content stay sharp on top.
//
// Dismissed via the Close button only — no tap-to-dismiss.

namespace {

constexpr int ALERT_WINDOW_S = 300;

struct CountdownState {
    lv_obj_t*   ring_widget;
    lv_obj_t*   scrim;
    lv_timer_t* timer;
    int         total_s;
    uint32_t    start_tick;
    int         last_label_s;
};

void countdown_tick(lv_timer_t* t) {
    auto* s = static_cast<CountdownState*>(lv_timer_get_user_data(t));
    int remaining_ms = s->total_s * 1000 - (int)lv_tick_elaps(s->start_tick);
    if (remaining_ms <= 0) {
        lv_obj_delete(s->scrim);
        return;
    }
    // Arc: degree resolution (0-360) moves ~1° per second vs 1° per 3 s with 0-100.
    uint16_t deg = (uint16_t)((uint32_t)remaining_ms * 360 / (ALERT_WINDOW_S * 1000));
    widget_progress_ring::set_angle(s->ring_widget, deg);
    // Label: only re-render when the displayed second changes.
    int remaining_s = (remaining_ms + 999) / 1000;
    if (remaining_s != s->last_label_s) {
        s->last_label_s = remaining_s;
        char label[8];
        snprintf(label, sizeof(label), "%d:%02d", remaining_s / 60, remaining_s % 60);
        widget_progress_ring::set_label_text_center(s->ring_widget, label);
    }
}

void on_scrim_delete(lv_event_t* e) {
    auto* s = static_cast<CountdownState*>(lv_event_get_user_data(e));
    if (s && s->timer) lv_timer_delete(s->timer);
    delete s;
}

} // namespace

namespace modal_countdown {

lv_obj_t* create(lv_obj_t* base_screen,
                 const char* meeting_title,
                 const char* when_text,
                 int seconds_remaining) {
    // Full-screen scrim that absorbs taps.
    lv_obj_t* scrim = lv_obj_create(base_screen);
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, theme::color(theme::COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, theme::SCRIM_OPA, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);

    // Content column, centered.
    lv_obj_t* col = lv_obj_create(scrim);
    lv_obj_set_size(col, 360, LV_SIZE_CONTENT);
    lv_obj_center(col);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);  // taps pass through to scrim
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Ring — initial position and label from seconds_remaining.
    char init_label[8];
    snprintf(init_label, sizeof(init_label), "%d:%02d",
             seconds_remaining / 60, seconds_remaining % 60);
    uint16_t init_deg = (uint16_t)((uint32_t)seconds_remaining * 360 / ALERT_WINDOW_S);

    lv_obj_t* ring = widget_progress_ring::create(col, 230);
    widget_progress_ring::set_angle(ring, init_deg);
    widget_progress_ring::set_label_font(ring, theme::font_large());
    widget_progress_ring::set_label_text_center(ring, init_label);
    widget_progress_ring::set_sub_label_text(ring, "UNTIL START");

    // Meeting name.
    lv_obj_t* name = lv_label_create(col);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_label_set_text(name, meeting_title);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_color(name, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(name, theme::font_title(), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(name, 20, 0);

    // When line.
    lv_obj_t* when = lv_label_create(col);
    lv_label_set_long_mode(when, LV_LABEL_LONG_WRAP);
    lv_label_set_text(when, when_text);
    lv_obj_set_width(when, lv_pct(100));
    lv_obj_set_style_text_color(when, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(when, theme::font_meta(), 0);
    lv_obj_set_style_text_align(when, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(when, 4, 0);

    // Gap between text and close button.
    lv_obj_t* gap = lv_obj_create(col);
    ui::clear_container(gap);
    lv_obj_set_size(gap, lv_pct(100), 18);

    lv_obj_t* close_btn = ui::make_btn(col, "Close", ui::BtnStyle::Tertiary,
                                       nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        lv_obj_delete(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
    }, LV_EVENT_CLICKED, scrim);

    // Self-contained 1 s timer drives the ring and label on every tick.
    auto* s = new CountdownState();
    s->ring_widget  = ring;
    s->scrim        = scrim;
    s->total_s      = seconds_remaining;
    s->start_tick   = lv_tick_get();
    s->last_label_s = seconds_remaining;
    s->timer        = lv_timer_create(countdown_tick, 1000, s);
    lv_obj_add_event_cb(scrim, on_scrim_delete, LV_EVENT_DELETE, s);

    return scrim;
}

} // namespace modal_countdown
