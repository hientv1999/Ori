#include "screens/modal_device_alert.h"

#include <lvgl.h>
#include <cstdio>

#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"

// See header + state-machine.md's "Weather Alert / Low Battery Alert
// overlays" section for the full picture. This file owns ONLY the overlay —
// built straight on lv_screen_active(), same structural pattern as
// modal_incoming_call.cpp — never the trigger-eval logic (that lives in
// state_machine.cpp's existing 1 s tick).

namespace {

constexpr int16_t NOTICE_CIRCLE_SIZE = 76;

// Accent-tinted "notice" circle — COLOR_ACCENT_SOFT/COLOR_ACCENT_LINE, the
// same non-error highlight pairing the media-mode toggle button uses for
// its "you are here" state (media-mode.md). Deliberately NOT
// ui::make_alert_glyph_circle() — that one is hardwired to
// COLOR_DANGER_SOFT/COLOR_DANGER, reserved for the factory-reset/unpair
// destructive-confirm flow. Caller fills the circle with whichever glyph
// applies (weather condition or battery level) and centers it.
lv_obj_t* make_notice_circle(lv_obj_t* parent) {
    lv_obj_t* circle = lv_obj_create(parent);
    lv_obj_set_size(circle, NOTICE_CIRCLE_SIZE, NOTICE_CIRCLE_SIZE);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, theme::color(theme::COLOR_ACCENT_SOFT), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(circle, theme::color(theme::COLOR_ACCENT_LINE), 0);
    lv_obj_set_style_border_width(circle, 2, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    return circle;
}

// Shared card body: notice circle + title + body text + single OK button.
// Mirrors ui::make_confirm_modal()'s layout, minus the Cancel/Danger pair —
// this only ever has one dismiss path. Writes the notice circle (for the
// caller to fill with its own glyph) to *out_circle.
// The live Low Battery overlay, if one is up. Tracked so the iPhone link
// dropping can tear it down (connectivity.md) — a battery reading is exactly
// the kind of claim that stops being verifiable the moment the phone is gone.
// Deliberately NOT tracked for the Weather Alert: that one is driven by
// Orion's weather poll and has nothing to do with the phone, so a phone
// disconnect must leave it alone. Cleared on the scrim's own LV_EVENT_DELETE,
// so a user-dismissed overlay never leaves a dangling pointer behind.
lv_obj_t* g_low_battery_scrim = nullptr;

lv_obj_t* build_overlay(const char* title, const char* body_text, lv_obj_t** out_circle) {
    ui::ModalLayout layout = ui::make_modal_layout(lv_screen_active(), 520, 400);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* card        = layout.card;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    // Absorb taps on the card itself so they don't fall through to the scrim
    // (which would dismiss) — same convention as modal_incoming_call.cpp.
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t*) {}, LV_EVENT_CLICKED, nullptr);

    ui::make_flex_spacer(scroll_area);

    *out_circle = make_notice_circle(scroll_area);

    lv_obj_t* h = lv_label_create(scroll_area);
    lv_label_set_text(h, title);
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_h2(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(h, 18, 0);

    lv_obj_t* b = lv_label_create(scroll_area);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_label_set_text(b, body_text);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_text_color(b, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(b, theme::font_meta(), 0);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(b, 10, 0);

    ui::make_flex_spacer(scroll_area);

    // Single OK button — Close-only, no tap-outside dismiss, same convention
    // as every other Ori detail overlay (state-machine.md).
    lv_obj_t* ok = ui::make_btn(actions, "OK", ui::BtnStyle::Tertiary,
                                nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(ok, ui::close_scrim_cb, LV_EVENT_CLICKED, scrim);
    return scrim;
}

} // namespace

namespace modal_device_alert {

void show_weather_alert(widget_profile_card::WeatherCondition condition, const char* end_time_str) {
    using WC = widget_profile_card::WeatherCondition;

    const char* title = "Rain expected";
    char body[192];
    const char* time_str = end_time_str ? end_time_str : "";

    switch (condition) {
        case WC::Thunderstorm:
            title = "Thunderstorm expected";
            snprintf(body, sizeof(body),
                     "A thunderstorm is expected around the time your work day ends at %s.",
                     time_str);
            break;
        case WC::Snow:
            title = "Snow expected";
            snprintf(body, sizeof(body),
                     "Snow is expected around the time your work day ends at %s.",
                     time_str);
            break;
        case WC::Rain:
        default:
            // Only Rain/Thunderstorm/Snow ever reach here (state_machine.cpp's
            // trigger check) — Rain is also the sane fallback for anything else.
            title = "Rain expected";
            snprintf(body, sizeof(body),
                     "Rain is expected around the time your work day ends at %s.",
                     time_str);
            break;
    }

    lv_obj_t* circle = nullptr;
    build_overlay(title, body, &circle);
    // Fixed is_night=false / intensity=Moderate — a deliberate simplification
    // matching what the approved Ori_UI_Prototype.html "weather-alert" screen
    // also hardcodes (state-machine.md).
    widget_profile_card::create_weather_glyph(circle, condition, /*is_night=*/false,
                                               widget_profile_card::WeatherIntensity::Moderate);
}

void show_low_battery_alert(const char* phone_kind_word, const char* phone_name,
                             uint8_t battery_pct) {
    const char* kind = (phone_kind_word && phone_kind_word[0]) ? phone_kind_word : "iPhone";
    char title[48];
    snprintf(title, sizeof(title), "%s Battery Low", kind);

    const char* name = (phone_name && phone_name[0]) ? phone_name : kind;
    char body[192];
    snprintf(body, sizeof(body), "%s is at %u%% battery.", name, (unsigned)battery_pct);

    lv_obj_t* circle = nullptr;
    lv_obj_t* scrim  = build_overlay(title, body, &circle);
    lv_obj_t* glyph  = ui::make_battery_glyph(circle, battery_pct);
    lv_obj_center(glyph);

    // Only this overlay is tracked — see g_low_battery_scrim. Identity-guarded
    // like every other self-nulling delete handler in the codebase, so a newer
    // overlay's pointer can't be cleared by an older one's teardown.
    g_low_battery_scrim = scrim;
    lv_obj_add_event_cb(scrim, [](lv_event_t* e) {
        if (static_cast<lv_obj_t*>(lv_event_get_target(e)) == g_low_battery_scrim)
            g_low_battery_scrim = nullptr;
    }, LV_EVENT_DELETE, nullptr);
}

void close_low_battery_alert() {
    if (g_low_battery_scrim) lv_obj_delete(g_low_battery_scrim);  // delete cb nulls it
}

} // namespace modal_device_alert
