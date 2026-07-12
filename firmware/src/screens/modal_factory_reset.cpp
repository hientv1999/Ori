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

} // namespace

namespace modal_factory_reset {

lv_obj_t* create(lv_obj_t* base_screen) {
    return ui::make_confirm_modal(base_screen,
        "Factory reset Ori?",
        "All data and paired devices will be removed",
        "Reset", on_reset);
}

} // namespace modal_factory_reset
