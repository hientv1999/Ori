#include "screens/screen_pto.h"

#include <lvgl.h>

#include "mock_data.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// PTO scenic.
//
// Prototype uses a stacked CSS gradient + SVG mountain silhouettes to evoke
// a Lisbon-ish sunset. LVGL has neither SVG nor stacked gradients, but
// solid color blocks with vertical gradients capture the same visual rhythm:
//
//   [0..40%]  sky:    deep blue   → cooler blue
//   [40..70%] sun:    warm peach
//   [70..85%] sand:   peach
//   [85..100%] water: dark blue   → near-black
//
// Real scenic JPEG decode arrives in M8 (LV_USE_SJPG) — the BLE protocol
// already carries the PtoEntry.image bytes.
//
// A light vignette (50% panel height, LV_OPA_30) preserves atmosphere
// without obscuring the scene. The frosted card at the bottom (18 px inset
// from all edges) shows "ON PTO", destination, and dates; tapping it opens
// a full-screen detail modal.

namespace {

// Stable pointers into mock_data::pto() strings — set before the card is
// created so the card's click callback can read them without capturing.
static const char* s_destination = nullptr;
static const char* s_dates       = nullptr;

lv_obj_t* make_band(lv_obj_t* parent, int16_t y, int16_t h,
                    uint32_t top, uint32_t bottom) {
    lv_obj_t* band = lv_obj_create(parent);
    lv_obj_set_size(band, lv_pct(100), h);
    lv_obj_set_pos(band, 0, y);
    lv_obj_set_style_bg_color(band, theme::color(top), 0);
    lv_obj_set_style_bg_grad_color(band, theme::color(bottom), 0);
    lv_obj_set_style_bg_grad_dir(band, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(band, 0, 0);
    lv_obj_set_style_pad_all(band, 0, 0);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
    return band;
}

lv_obj_t* make_sun(lv_obj_t* parent) {
    lv_obj_t* sun = lv_obj_create(parent);
    lv_obj_set_size(sun, 110, 110);
    lv_obj_align(sun, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sun, theme::color(0xF4D293), 0);
    lv_obj_set_style_bg_grad_color(sun, theme::color(0xE8B974), 0);
    lv_obj_set_style_bg_grad_dir(sun, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(sun, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sun, 0, 0);
    lv_obj_set_style_pad_all(sun, 0, 0);
    lv_obj_set_style_shadow_color(sun, theme::color(0xF4D293), 0);
    lv_obj_set_style_shadow_width(sun, 30, 0);
    lv_obj_set_style_shadow_opa(sun, LV_OPA_40, 0);
    lv_obj_clear_flag(sun, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sun, LV_OBJ_FLAG_CLICKABLE);
    return sun;
}

// Full-screen detail modal — closed only via the Close button.
static void show_pto_detail(lv_obj_t* screen) {
    // Scrim — absorbs taps behind the dialog; no click-to-dismiss.
    lv_obj_t* scrim = lv_obj_create(screen);
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, theme::color(theme::COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, theme::SCRIM_OPA, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);

    // Outer box — width 480, height wraps content, capped at 400 px so the
    // box never runs off screen on small-text or large-font configurations.
    lv_obj_t* box = lv_obj_create(scrim);
    lv_obj_set_width(box, 480);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, 400, 0);
    lv_obj_center(box);
    ui::clear_container(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Scrollable content area — eyebrow, destination, dates.
    // max_height = 400 (box cap) − 28 (gap) − 60 (close btn) = 312 px.
    lv_obj_t* scroll = lv_obj_create(box);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_height(scroll, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(scroll, 312, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // "ON PTO" eyebrow.
    lv_obj_t* eyebrow = lv_label_create(scroll);
    lv_label_set_text_static(eyebrow, "ON PTO");
    lv_obj_set_style_text_font(eyebrow, theme::font_meta(), 0);
    lv_obj_set_style_text_color(eyebrow, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_letter_space(eyebrow, 4, 0);
    lv_obj_set_style_text_align(eyebrow, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(eyebrow, lv_pct(100));
    lv_label_set_long_mode(eyebrow, LV_LABEL_LONG_WRAP);

    // Destination.
    lv_obj_t* dest = lv_label_create(scroll);
    lv_label_set_text(dest, s_destination ? s_destination : "");
    lv_label_set_long_mode(dest, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(dest, lv_pct(100));
    lv_obj_set_style_text_font(dest, theme::font_display(), 0);
    lv_obj_set_style_text_color(dest, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(dest, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(dest, 12, 0);

    // Date range.
    lv_obj_t* dates = lv_label_create(scroll);
    lv_label_set_text(dates, s_dates ? s_dates : "");
    lv_label_set_long_mode(dates, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(dates, lv_pct(100));
    lv_obj_set_style_text_font(dates, theme::font_meta(), 0);
    lv_obj_set_style_text_color(dates, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(dates, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(dates, 8, 0);

    // Gap between scroll area and close button (LVGL has no margin style).
    lv_obj_t* gap = lv_obj_create(box);
    ui::clear_container(gap);
    lv_obj_set_size(gap, lv_pct(100), 28);

    lv_obj_t* close_btn = ui::make_btn(box, "Close", ui::BtnStyle::Tertiary,
                                       nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        lv_obj_delete(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
    }, LV_EVENT_CLICKED, scrim);
}

} // namespace

namespace screen_pto {

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    widget_status_bar::create(screen);

    lv_obj_t* body = ui::make_screen_body(screen);

    // Left panel — scenic. 528 x 396.
    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, 528, lv_pct(100));
    lv_obj_set_style_bg_color(left, theme::color(0x2A4565), 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_CLICKABLE);

    constexpr int16_t H = 480 - widget_status_bar::HEIGHT;  // 396

    // Sky.
    make_band(left, 0, H * 35 / 100, 0x2A4565, 0x45617E);
    // Warm strip.
    make_band(left, H * 35 / 100, H * 30 / 100, 0x45617E, 0xC08868);
    // Sand.
    make_band(left, H * 65 / 100, H * 20 / 100, 0xC08868, 0xD9A47A);
    // Water.
    make_band(left, H * 77 / 100, H - H * 77 / 100, 0x1F3A55, 0x0E1F30);

    make_sun(left);

    // Light vignette — bottom 50% of the panel, LV_OPA_30 so the scene shows through.
    lv_obj_t* vignette = lv_obj_create(left);
    lv_obj_set_size(vignette, lv_pct(100), H * 50 / 100);
    lv_obj_set_pos(vignette, 0, H * 50 / 100);
    lv_obj_set_style_bg_color(vignette, theme::color(0x0E1116), 0);
    lv_obj_set_style_bg_opa(vignette, LV_OPA_30, 0);
    lv_obj_set_style_border_width(vignette, 0, 0);
    lv_obj_set_style_pad_all(vignette, 0, 0);
    lv_obj_clear_flag(vignette, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(vignette, LV_OBJ_FLAG_CLICKABLE);

    // Stash string pointers for the detail modal before creating the card.
    const auto& p = mock_data::pto();
    s_destination = p.destination;
    s_dates       = p.range_label;

    // Frosted dark card — anchored 18 px from the bottom and sides.
    // Width = 528 - 36 = 492 px; height grows with content.
    lv_obj_t* card = lv_obj_create(left);
    lv_obj_set_size(card, 492, LV_SIZE_CONTENT);
    lv_obj_set_pos(card, 18, H - 18);   // bottom edge at H-18; LVGL positions from top-left
    // Re-anchor using alignment so the bottom of the card sits at H-18.
    lv_obj_align(card, LV_ALIGN_BOTTOM_LEFT, 18, -18);
    lv_obj_set_style_bg_color(card, theme::color(0x0A0C10), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_color(card, theme::color(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_top(card, 14, 0);
    lv_obj_set_style_pad_bottom(card, 18, 0);
    lv_obj_set_style_pad_left(card, 24, 0);
    lv_obj_set_style_pad_right(card, 24, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Card children — flex column, centred horizontally.

    // Eyebrow: "ON PTO"
    lv_obj_t* eyebrow = lv_label_create(card);
    lv_label_set_text_static(eyebrow, "ON PTO");
    lv_obj_set_style_text_font(eyebrow, theme::font_meta(), 0);
    lv_obj_set_style_text_color(eyebrow, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_letter_space(eyebrow, 4, 0);

    // Destination — single line with ellipsis, full card width.
    lv_obj_t* dest = lv_label_create(card);
    lv_label_set_long_mode(dest, LV_LABEL_LONG_DOT);
    lv_obj_set_width(dest, lv_pct(100));
    lv_label_set_text(dest, p.destination);
    lv_obj_set_style_text_font(dest, theme::font_display(), 0);
    lv_obj_set_style_text_color(dest, theme::color(0xFFFFFF), 0);
    lv_obj_set_style_text_align(dest, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(dest, 4, 0);

    // Date range.
    lv_obj_t* dates = lv_label_create(card);
    lv_label_set_text(dates, p.range_label);
    lv_obj_set_style_text_font(dates, theme::font_meta(), 0);
    lv_obj_set_style_text_color(dates, theme::color(0xCCCCCC), 0);
    lv_obj_set_style_text_align(dates, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(dates, 4, 0);

    // Card click → full-screen detail modal.
    lv_obj_add_event_cb(card, [](lv_event_t* e) {
        show_pto_detail(lv_obj_get_screen((lv_obj_t*)lv_event_get_target(e)));
    }, LV_EVENT_CLICKED, nullptr);

    ui::make_panel_divider(body);
    widget_profile_card::create(body);
    return screen;
}

} // namespace screen_pto
