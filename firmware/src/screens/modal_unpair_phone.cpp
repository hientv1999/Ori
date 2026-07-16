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
    // Personalise with the connected phone's GAP device name when available
    // ("Xander's iPhone"); fall back to "your iPhone"/"your iPad" (or the
    // generic "your iPhone or iPad" before the bond's model is known) when
    // not. lv_label_set_text copies the buffer, so stack storage is fine.
    const char* pname = ancs_client::phone_name();
    const char* kind = ancs_client::phone_kind_word();

    char generic_buf[24];
    snprintf(generic_buf, sizeof(generic_buf), "your %s", kind);

    char body_buf[160];
    snprintf(body_buf, sizeof(body_buf),
             "Ori will no longer show notifications from %s",
             (pname && pname[0]) ? pname : generic_buf);

    char title_buf[32];
    snprintf(title_buf, sizeof(title_buf), "Unpair %s?", kind);

    // M8: replace the icon with a proper phone asset — shares the same alert
    // circle shape as modal_factory_reset's warning glyph (make_confirm_modal).
    return ui::make_confirm_modal(base_screen, title_buf, body_buf,
        "Unpair", on_unpair_confirm);
}

} // namespace modal_unpair_phone
