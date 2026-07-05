#include "screens/screen_time_off.h"

#include <lvgl.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include "nvs_sync.h"
#include "photo_cache.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Time Off scenic screen.
//
// Background: real destination JPEG from photo_cache::get_time_off() if available,
// otherwise the gradient placeholder bands.
// Text: real Time Off metadata from NVS (start/end/destination). Buffers stay empty
// when no NVS entry exists — the state machine only enters Time Off when the window is active.

namespace {

// String storage for detail modal (set in create() before modal can open).
static char s_destination[129] = {};
static char s_dates[64]        = {};


// ── Gradient fallback ─────────────────────────────────────────────────────────

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

void build_gradient_bg(lv_obj_t* left, int16_t H) {
    make_band(left, 0,           H * 35 / 100, 0x2A4565, 0x45617E);
    make_band(left, H * 35 / 100, H * 30 / 100, 0x45617E, 0xC08868);
    make_band(left, H * 65 / 100, H * 20 / 100, 0xC08868, 0xD9A47A);
    make_band(left, H * 77 / 100, H - H * 77 / 100, 0x1F3A55, 0x0E1F30);
    make_sun(left);
}

// ── Detail modal ──────────────────────────────────────────────────────────────

static void show_time_off_detail(lv_obj_t* screen) {
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

    // Full-screen layout — matches modal_profile.cpp: content in flex_grow=1 area,
    // Close button pinned at fixed screen bottom (same Y as profile + ANCS overlays).
    lv_obj_t* box = lv_obj_create(scrim);
    lv_obj_set_size(box, 800, 480);
    lv_obj_set_pos(box, 0, 0);
    ui::clear_container(box);
    lv_obj_set_style_pad_top(box, widget_status_bar::HEIGHT, 0);
    lv_obj_set_style_pad_bottom(box, 24, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 480 px wide content column — grows to fill all height above the close button.
    // Spacers vertically centre the text block when it is shorter than the column.
    lv_obj_t* scroll = lv_obj_create(box);
    lv_obj_set_width(scroll, 480);
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* spacer_top = lv_obj_create(scroll);
    ui::clear_container(spacer_top);
    lv_obj_set_size(spacer_top, 0, 0);
    lv_obj_set_flex_grow(spacer_top, 1);

    lv_obj_t* eyebrow = lv_label_create(scroll);
    lv_label_set_text_static(eyebrow, "ON TIME OFF");
    lv_obj_set_style_text_font(eyebrow, theme::font_meta(), 0);
    lv_obj_set_style_text_color(eyebrow, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_letter_space(eyebrow, 4, 0);
    lv_obj_set_style_text_align(eyebrow, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(eyebrow, lv_pct(100));
    lv_label_set_long_mode(eyebrow, LV_LABEL_LONG_WRAP);

    lv_obj_t* dest = lv_label_create(scroll);
    lv_label_set_text(dest, s_destination);
    lv_label_set_long_mode(dest, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(dest, lv_pct(100));
    lv_obj_set_style_text_font(dest, theme::font_display(), 0);
    lv_obj_set_style_text_color(dest, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(dest, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(dest, 12, 0);

    lv_obj_t* dates = lv_label_create(scroll);
    lv_label_set_text(dates, s_dates);
    lv_label_set_long_mode(dates, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(dates, lv_pct(100));
    lv_obj_set_style_text_font(dates, theme::font_meta(), 0);
    lv_obj_set_style_text_color(dates, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(dates, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(dates, 8, 0);

    lv_obj_t* spacer_bot = lv_obj_create(scroll);
    ui::clear_container(spacer_bot);
    lv_obj_set_size(spacer_bot, 0, 0);
    lv_obj_set_flex_grow(spacer_bot, 1);

    // 28 px gap — matches modal_profile.cpp.
    lv_obj_t* gap = lv_obj_create(box);
    ui::clear_container(gap);
    lv_obj_set_size(gap, lv_pct(100), 28);

    lv_obj_t* close_btn = ui::make_btn(box, "Close", ui::BtnStyle::Tertiary,
                                       nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        lv_obj_delete(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
    }, LV_EVENT_CLICKED, scrim);
}

// ── Date range formatter ──────────────────────────────────────────────────────

static void format_date_range(char* out, size_t out_sz,
                               uint32_t start_epoch, uint32_t end_epoch) {
    time_t st = (time_t)start_epoch;
    time_t et = (time_t)end_epoch;
    struct tm stm = {}, etm = {};
    gmtime_r(&st, &stm);
    gmtime_r(&et, &etm);
    // "Jun 10 – Jul 5, 2026"
    char s1[16] = {}, s2[24] = {};
    strftime(s1, sizeof(s1), "%b %d", &stm);
    strftime(s2, sizeof(s2), "%b %d, %Y", &etm);
    snprintf(out, out_sz, "%s \xe2\x80\x93 %s", s1, s2); // UTF-8 en-dash
}

} // namespace

namespace screen_time_off {

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    widget_status_bar::create(screen);

    lv_obj_t* body = ui::make_screen_body(screen);
    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, 528, lv_pct(100));
    lv_obj_set_style_bg_color(left, theme::color(0x2A4565), 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_CLICKABLE);

    constexpr int16_t H = 480 - widget_status_bar::HEIGHT; // 396

    // ── Background: BLE image → compiled-in placeholder → gradient ────────
    // Priority: real BLE image (528×396 from Orion) > compiled-in placeholder
    // > painted gradient bands. The placeholder is a JPEG baked into firmware
    // flash at build time; see firmware/src/assets/time_off_placeholder.c.
    const lv_image_dsc_t* time_off_img = photo_cache::get_time_off();
    if (!time_off_img) time_off_img = photo_cache::get_time_off_placeholder();
    if (time_off_img) {
        lv_obj_t* img = lv_image_create(left);
        lv_image_set_src(img, time_off_img);
        lv_obj_set_pos(img, 0, 0);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        build_gradient_bg(left, H);
    }


    // ── Load Time Off text from NVS ───────────────────────────────────────
    uint32_t time_off_start = 0, time_off_end = 0;
    bool has_nvs = nvs_sync::load_time_off_meta(&time_off_start, &time_off_end,
                                                 s_destination, sizeof(s_destination));
    if (has_nvs && time_off_start && time_off_end) {
        format_date_range(s_dates, sizeof(s_dates), time_off_start, time_off_end);
    }
    if (!s_destination[0]) strncpy(s_destination, "Unknown destination", sizeof(s_destination) - 1);
    if (!s_dates[0])       strncpy(s_dates,       "Unknown period",      sizeof(s_dates) - 1);

    // ── Frosted info card ─────────────────────────────────────────────────
    lv_obj_t* card = lv_obj_create(left);
    lv_obj_set_size(card, 492, LV_SIZE_CONTENT);
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

    lv_obj_t* eyebrow = lv_label_create(card);
    lv_label_set_text_static(eyebrow, "ON TIME OFF");
    lv_obj_set_style_text_font(eyebrow, theme::font_meta(), 0);
    lv_obj_set_style_text_color(eyebrow, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_letter_space(eyebrow, 4, 0);

    lv_obj_t* dest_lbl = lv_label_create(card);
    lv_label_set_long_mode(dest_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(dest_lbl, lv_pct(100));
    lv_label_set_text(dest_lbl, s_destination);
    lv_obj_set_style_text_font(dest_lbl, theme::font_display(), 0);
    lv_obj_set_style_text_color(dest_lbl, theme::color(0xFFFFFF), 0);
    lv_obj_set_style_text_align(dest_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(dest_lbl, 4, 0);

    lv_obj_t* dates_lbl = lv_label_create(card);
    lv_label_set_text(dates_lbl, s_dates);
    lv_obj_set_style_text_font(dates_lbl, theme::font_meta(), 0);
    lv_obj_set_style_text_color(dates_lbl, theme::color(0xCCCCCC), 0);
    lv_obj_set_style_text_align(dates_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(dates_lbl, 4, 0);

    lv_obj_add_event_cb(card, [](lv_event_t* e) {
        show_time_off_detail(lv_obj_get_screen((lv_obj_t*)lv_event_get_target(e)));
    }, LV_EVENT_CLICKED, nullptr);

    ui::make_panel_divider(body);
    widget_profile_card::create(body);
    return screen;
}

} // namespace screen_time_off
