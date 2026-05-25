#include "screens/screen_repair_phone.h"

#include <Arduino.h>
#include <lvgl.h>

#include "mock_data.h"
#include "state_machine.h"
#include "theme.h"
#include "ui_helpers.h"

// Runtime re-pair phone screen.
//
// Pixel-identical to Step 4 (Phone pairing) per setup-flow.md — status bar
// is hidden so the loading spinner has the same room. The ONLY difference
// is the action button reads "Cancel" instead of "Skip for now", and tapping
// it calls state_machine::evaluate() to restore the correct runtime screen.

namespace {

lv_obj_t* make_ble_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* pill = lv_obj_create(parent);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, theme::color(theme::COLOR_ACCENT_SOFT), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 16, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_left(pill, 32, 0);
    lv_obj_set_style_pad_right(pill, 32, 0);
    lv_obj_set_style_pad_top(pill, 16, 0);
    lv_obj_set_style_pad_bottom(pill, 16, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(pill);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(lbl, theme::font_large(), 0);
    lv_obj_center(lbl);
    return pill;
}

constexpr uint32_t SPIN_INTERVAL_MS = 42;  // 24 fps
constexpr uint16_t SPIN_STEP_DEG    = 10;  // 10° × 36 steps × 42 ms ≈ 1512 ms/rev

struct SpinnerState {
    uint16_t    rotation;
    lv_timer_t* timer;
};

static void spinner_timer_cb(lv_timer_t* t) {
    lv_obj_t* arc = static_cast<lv_obj_t*>(t->user_data);
    auto* s = static_cast<SpinnerState*>(lv_obj_get_user_data(arc));
    s->rotation = (uint16_t)((s->rotation + SPIN_STEP_DEG) % 360);
    lv_arc_set_rotation(arc, s->rotation);
}

lv_obj_t* make_spinner(lv_obj_t* parent, int16_t size) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, theme::color(theme::COLOR_DIVIDER_STRONG), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, theme::color(theme::COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, 0, 60);
    auto* s = new SpinnerState{0, nullptr};
    lv_obj_set_user_data(arc, s);
    s->timer = lv_timer_create(spinner_timer_cb, SPIN_INTERVAL_MS, arc);
    lv_obj_add_event_cb(arc, [](lv_event_t* e) {
        auto* ss = static_cast<SpinnerState*>(
            lv_obj_get_user_data(lv_event_get_target(e)));
        if (ss) { lv_timer_del(ss->timer); delete ss; }
    }, LV_EVENT_DELETE, nullptr);
    return arc;
}

} // namespace

namespace screen_repair_phone {

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    // Single content column — status bar HIDDEN (we just don't create one).
    lv_obj_t* content = lv_obj_create(screen);
    lv_obj_set_size(content, 800, 480);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_set_style_bg_color(content, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_top(content, 12, 0);
    lv_obj_set_style_pad_bottom(content, 12, 0);
    lv_obj_set_style_pad_left(content, 0, 0);
    lv_obj_set_style_pad_right(content, 0, 0);
    lv_obj_set_style_pad_row(content, 0, 0);
    lv_obj_set_style_pad_column(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* h = lv_label_create(content);
    lv_label_set_text(h, "Phone pairing");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* p = lv_label_create(content);
    lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
    lv_label_set_text(p,
        "Ori provides quiet notification awareness via Bluetooth connection");
    lv_obj_set_width(p, 800);
    lv_obj_set_style_text_color(p, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p, theme::font_meta(), 0);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p, 16, 0);
    lv_obj_set_style_pad_bottom(p, 16, 0);

    lv_obj_t* pill = make_ble_pill(content, mock_data::ble_name());
    lv_obj_set_style_pad_top(pill, 22, 0);

    lv_obj_t* spinner = make_spinner(content, 100);
    lv_obj_set_style_pad_top(spinner, 24, 0);

    lv_obj_t* hint = lv_label_create(content);
    lv_label_set_text(hint, "Ori will continue automatically once phone is connected");
    lv_obj_set_style_text_color(hint, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(hint, theme::font_meta(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(hint, 14, 0);
    lv_obj_set_style_pad_bottom(hint, 18, 0);

    // Cancel: return to the correct runtime screen by re-running the state
    // machine.  Closure captures the screen pointer so we can delete it
    // after loading the replacement.
    lv_obj_t* cancel_btn = ui::make_btn(content, "Cancel",
        ui::BtnStyle::Secondary,
        nullptr, nullptr,
        14, 56, theme::font_time());
    lv_obj_add_event_cb(cancel_btn, [](lv_event_t* /*e*/) {
        Serial.println("[repair_phone] Cancel — returning to runtime");
        // Force a full re-evaluate; the state machine will delete this screen
        // and load the right one.
        state_machine::evaluate();
    }, LV_EVENT_CLICKED, nullptr);

    return screen;
}

} // namespace screen_repair_phone
