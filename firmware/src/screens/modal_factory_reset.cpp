#include "screens/modal_factory_reset.h"

#include <Arduino.h>
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
    Serial.println("[modal_factory_reset] Reset confirmed — deferring execute");
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
    lv_obj_t* scrim = lv_obj_create(base_screen);
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, theme::color(theme::COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, theme::SCRIM_OPA, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);  // absorb taps outside the card

    // Card.
    lv_obj_t* card = lv_obj_create(scrim);
    lv_obj_set_size(card, 520, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, theme::color(theme::COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, theme::color(theme::COLOR_DIVIDER_STRONG), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_shadow_color(card, theme::color(0x000000), 0);
    lv_obj_set_style_shadow_width(card, 30, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(card, 32, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_warn_circle(card);

    lv_obj_t* heading = lv_label_create(card);
    lv_label_set_text(heading, "Factory reset Ori?");
    lv_obj_set_style_text_color(heading, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(heading, theme::font_h2(), 0);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(heading, 18, 0);

    lv_obj_t* body = lv_label_create(card);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body,
        "All data and paired devices will be removed");
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(body, theme::font_meta(), 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(body, 10, 0);

    // Actions row.
    lv_obj_t* actions = lv_obj_create(card);
    lv_obj_set_size(actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_top(actions, 22, 0);
    lv_obj_set_style_pad_column(actions, 14, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* cancel = ui::make_btn(actions, "Cancel", ui::BtnStyle::Secondary,
                                    nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(cancel, on_cancel, LV_EVENT_CLICKED, scrim);

    lv_obj_t* reset = ui::make_btn(actions, "Reset", ui::BtnStyle::Danger,
                                   nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(reset, on_reset, LV_EVENT_CLICKED, scrim);

    return scrim;
}

} // namespace modal_factory_reset
