#include "screens/modal_countdown.h"

#include <cstdio>
#include <lvgl.h>

#include "state_machine.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_progress_ring.h"

// 5-minute pre-meeting countdown modal.
//
// Visual reference: prototype `.modal-scrim` + `.countdown` block. A blurred
// scrim isn't representable in LVGL's RGB565 rendering without a custom
// shader, so we fall back to a heavy opacity dimming (LV_OPA_80 of
// COLOR_SCRIM) — visually close to the prototype's backdrop-filter at the
// read distance of a desk display. The ring + content stay sharp on top.
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
    // Single choke point for every dismissal path (Close tap, zero-timeout
    // auto-dismiss, or the modal being torn down by a screen swap) — clears
    // the state machine's COUNTDOWN sentinel so it doesn't stay wedged there.
    state_machine::on_countdown_close();
}

} // namespace

namespace modal_countdown {

lv_obj_t* create(lv_obj_t* base_screen,
                 const char* meeting_title,
                 const char* organizer,
                 const char* location,
                 int seconds_remaining) {
    lv_obj_t* scrim = ui::make_scrim(base_screen);

    // Content column, centered. Full screen width (with side padding) so the
    // title / organizer / location labels below — each lv_pct(100), single-line
    // with ellipsis — can use nearly the whole screen before truncating, instead
    // of being capped at a narrow column. The ring + Close button stay centred.
    lv_obj_t* col = lv_obj_create(scrim);
    lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_center(col);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_hor(col, 24, 0);  // side margin so text isn't edge-to-edge
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

    // Meeting title — single line, ellipsis on overflow.
    lv_obj_t* name = lv_label_create(col);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, meeting_title);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_color(name, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(name, theme::font_title(), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(name, 20, 0);

    // Organizer — single line, ellipsis. Omitted when empty.
    if (organizer && organizer[0]) {
        lv_obj_t* org = lv_label_create(col);
        lv_label_set_long_mode(org, LV_LABEL_LONG_DOT);
        lv_label_set_text(org, organizer);
        lv_obj_set_width(org, lv_pct(100));
        lv_obj_set_style_text_color(org, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(org, theme::font_meta(), 0);
        lv_obj_set_style_text_align(org, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(org, 3, 0);  // gap above organizer (was 6, −50%)
    }

    // Location — single line, ellipsis. Omitted when empty.
    if (location && location[0]) {
        lv_obj_t* loc = lv_label_create(col);
        lv_label_set_long_mode(loc, LV_LABEL_LONG_DOT);
        lv_label_set_text(loc, location);
        lv_obj_set_width(loc, lv_pct(100));
        lv_obj_set_style_text_color(loc, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
        lv_obj_set_style_text_font(loc, theme::font_meta(), 0);
        lv_obj_set_style_text_align(loc, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(loc, 2, 0);  // gap above location (was 4, −50%)
    }

    // Gap between text and close button.
    lv_obj_t* gap = lv_obj_create(col);
    ui::clear_container(gap);
    lv_obj_set_size(gap, lv_pct(100), 4);

    lv_obj_t* close_btn = ui::make_btn(col, "Close", ui::BtnStyle::Tertiary,
                                       nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(close_btn, ui::close_scrim_cb, LV_EVENT_CLICKED, scrim);

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
