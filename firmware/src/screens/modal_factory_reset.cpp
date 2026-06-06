#include "screens/modal_factory_reset.h"

#include <Arduino.h>
#include "ori_log.h"
#include <lvgl.h>

#include "factory_reset.h"
#include "state_machine.h"
#include "theme.h"
#include "ui_helpers.h"

// Factory reset confirmation. Centered alert card on top of a scrim.
//
// Layout matches prototype `.alert-card.reset`:
//   - 520 px wide card
//   - 96 x 96 warning icon circle (danger color at 14%, danger glyph)
//   - "Factory reset Ori?" heading
//   - Body copy explaining what's wiped
//   - Cancel (secondary) + Reset (danger) actions
//
// Cancel dismisses the modal; Reset calls state_machine::on_factory_reset()
// which wipes NVS and calls ESP.restart().

namespace {

void on_cancel(lv_event_t* e) {
    lv_obj_t* scrim = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_obj_delete(scrim);
}

// One-shot deferred timer so factory_reset::execute() is not called from
// inside an LVGL event callback (avoids the LCD DMA ISR / NVS cache fault
// documented in CLAUDE.md known-bugs for M4).
static void factory_reset_timer_cb(lv_timer_t* t) {
    lv_timer_delete(t);
    factory_reset::execute();
}

void on_reset(lv_event_t* /*e*/) {
    LOG("[modal_factory_reset] Reset confirmed — deferring execute\n");
    lv_timer_create(factory_reset_timer_cb, 50, nullptr);
}

lv_obj_t* make_warn_circle(lv_obj_t* parent) {
    lv_obj_t* circle = lv_obj_create(parent);
    lv_obj_set_size(circle, 96, 96);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, theme::color(theme::COLOR_DANGER_SOFT), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);

    // "!" glyph — light-weight stand-in for the prototype's warning triangle.
    lv_obj_t* bang = lv_label_create(circle);
    lv_label_set_text(bang, "!");
    lv_obj_set_style_text_color(bang, theme::color(theme::COLOR_DANGER), 0);
    lv_obj_set_style_text_font(bang, theme::font_large(), 0);
    lv_obj_center(bang);
    return circle;
}

} // namespace

namespace modal_factory_reset {

lv_obj_t* create(lv_obj_t* base_screen) {
    ui::ModalLayout layout = ui::make_modal_layout(base_screen);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    lv_obj_t* spacer_top = lv_obj_create(scroll_area);
    ui::clear_container(spacer_top);
    lv_obj_set_size(spacer_top, 0, 0);
    lv_obj_set_flex_grow(spacer_top, 1);

    make_warn_circle(scroll_area);

    lv_obj_t* heading = lv_label_create(scroll_area);
    lv_label_set_text(heading, "Factory reset Ori?");
    lv_obj_set_style_text_color(heading, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(heading, theme::font_h2(), 0);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(heading, 18, 0);

    lv_obj_t* body = lv_label_create(scroll_area);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body,
        "All data and paired devices will be removed");
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(body, theme::font_meta(), 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(body, 10, 0);

    lv_obj_t* spacer_bot = lv_obj_create(scroll_area);
    ui::clear_container(spacer_bot);
    lv_obj_set_size(spacer_bot, 0, 0);
    lv_obj_set_flex_grow(spacer_bot, 1);

    // Danger action on the left — users instinctively tap the right button,
    // so placing the destructive action on the left reduces accidental presses.
    lv_obj_t* reset = ui::make_btn(actions, "Reset", ui::BtnStyle::Danger,
                                   nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(reset, on_reset, LV_EVENT_CLICKED, scrim);

    lv_obj_t* cancel = ui::make_btn(actions, "Cancel", ui::BtnStyle::Tertiary,
                                    nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(cancel, on_cancel, LV_EVENT_CLICKED, scrim);

    return scrim;
}

} // namespace modal_factory_reset
