#include "screens/modal_unpair_phone.h"

#include <Arduino.h>
#include "ori_log.h"
#include <lvgl.h>
#include <cstdio>

#include "ble/ancs_client.h"
#include "state_machine.h"
#include "theme.h"
#include "ui_helpers.h"

// Unpair phone confirmation modal. Centered alert card on top of a scrim.
//
// Layout matches modal_factory_reset — same card width, same icon circle,
// same button row — adapted for the phone-unpairing context.
//
// Cancel dismisses; Unpair calls state_machine::on_unpair_phone(), which
// wipes the phone bond. The user re-pairs later by tapping the phone icon —
// unpairing does not auto-navigate to a re-pair screen (see below).

namespace {

void on_cancel(lv_event_t* e) {
    lv_obj_t* scrim = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_obj_delete(scrim);
}

void on_unpair_confirm(lv_event_t* e) {
    lv_obj_t* scrim = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_obj_delete(scrim);
    LOG("[modal_unpair_phone] Unpair confirmed\n");
    // See module header comment above re: re-pair navigation.
    state_machine::on_unpair_phone();
}

} // namespace

namespace modal_unpair_phone {

lv_obj_t* create(lv_obj_t* base_screen) {
    ui::ModalLayout layout = ui::make_modal_layout(base_screen);
    lv_obj_t* scrim      = layout.scrim;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions    = layout.actions;

    lv_obj_t* spacer_top = lv_obj_create(scroll_area);
    ui::clear_container(spacer_top);
    lv_obj_set_size(spacer_top, 0, 0);
    lv_obj_set_flex_grow(spacer_top, 1);

    // M8: replace with a proper phone icon asset — shares the same alert
    // circle shape as modal_factory_reset's warning glyph.
    ui::make_alert_glyph_circle(scroll_area);

    lv_obj_t* heading = lv_label_create(scroll_area);
    lv_label_set_text(heading, "Unpair iPhone?");
    lv_obj_set_style_text_color(heading, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(heading, theme::font_h2(), 0);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(heading, 18, 0);

    // Personalise with the connected phone's GAP device name when available
    // ("Xander's iPhone"); fall back to the generic copy when not.
    // lv_label_set_text copies the buffer, so stack storage is fine.
    const char* pname = ancs_client::phone_name();
    char body_buf[160];
    snprintf(body_buf, sizeof(body_buf),
             "Ori will no longer show notifications from %s",
             (pname && pname[0]) ? pname : "your iPhone");

    lv_obj_t* body = lv_label_create(scroll_area);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body, body_buf);
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
    lv_obj_t* unpair = ui::make_btn(actions, "Unpair", ui::BtnStyle::Danger,
                                    nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(unpair, on_unpair_confirm, LV_EVENT_CLICKED, scrim);

    lv_obj_t* cancel = ui::make_btn(actions, "Cancel", ui::BtnStyle::Tertiary,
                                    nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(cancel, on_cancel, LV_EVENT_CLICKED, scrim);

    return scrim;
}

} // namespace modal_unpair_phone
