#include "screens/modal_profile.h"

#include <lvgl.h>

#include "mock_data.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"  // for WIDTH, PHOTO_SIZE, get_default_presence

namespace modal_profile {

// Status-bar height — body row (and right panel) start at this Y offset.
static constexpr lv_coord_t STATUS_BAR_H = 84;

lv_obj_t* create(lv_obj_t* base_screen, lv_obj_t* ref_photo) {
    const mock_data::Profile& p = mock_data::profile();

    // Presence border colour — same swatch as the live profile card.
    widget_profile_card::Presence pres = widget_profile_card::get_default_presence();
    uint32_t pres_color;
    switch (pres) {
        case widget_profile_card::Presence::Available: pres_color = theme::COLOR_PRESENCE_AVAILABLE; break;
        case widget_profile_card::Presence::Busy:      pres_color = theme::COLOR_PRESENCE_BUSY;      break;
        case widget_profile_card::Presence::Away:      pres_color = theme::COLOR_PRESENCE_AWAY;      break;
        default:                                       pres_color = theme::COLOR_PRESENCE_OFFLINE;   break;
    }

    // Full-screen scrim — absorbs taps, no dismiss.
    lv_obj_t* scrim = lv_obj_create(base_screen);
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, theme::color(theme::COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, theme::SCRIM_OPA, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);

    // Outer box — full screen, flex column: [body row] + [close button].
    // pad_top = STATUS_BAR_H so the body row is flush with where the right
    // panel starts, making the photo Y coordinate directly computable.
    lv_obj_t* box = lv_obj_create(scrim);
    lv_obj_set_size(box, 800, 480);
    lv_obj_set_pos(box, 0, 0);
    ui::clear_container(box);
    lv_obj_set_style_pad_top(box, STATUS_BAR_H, 0);
    lv_obj_set_style_pad_bottom(box, 24, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t* body_row = lv_obj_create(box);
    lv_obj_set_width(body_row, lv_pct(100));
    lv_obj_set_flex_grow(body_row, 1);
    ui::clear_container(body_row);
    lv_obj_set_flex_flow(body_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // ── Left column — scrollable info block ──
    // Centering spacers keep content vertically centred when it fits;
    // they collapse when overflowing so scrolling starts from the top.
    lv_obj_t* left_col = lv_obj_create(body_row);
    lv_obj_set_flex_grow(left_col, 1);
    lv_obj_set_height(left_col, lv_pct(100));
    lv_obj_set_style_bg_opa(left_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_col, 0, 0);
    lv_obj_set_style_pad_left(left_col, 40, 0);
    lv_obj_set_style_pad_right(left_col, 20, 0);
    lv_obj_set_style_pad_top(left_col, 0, 0);
    lv_obj_set_style_pad_bottom(left_col, 0, 0);
    lv_obj_add_flag(left_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(left_col, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(left_col, LV_DIR_VER);
    lv_obj_set_flex_flow(left_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* spacer_top = lv_obj_create(left_col);
    ui::clear_container(spacer_top);
    lv_obj_set_size(spacer_top, 0, 0);
    lv_obj_set_flex_grow(spacer_top, 1);

    lv_obj_t* name_lbl = lv_label_create(left_col);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(name_lbl, p.name);
    lv_obj_set_width(name_lbl, lv_pct(100));
    lv_obj_set_style_text_font(name_lbl, theme::font_h2(), 0);
    lv_obj_set_style_text_color(name_lbl, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* title_lbl = lv_label_create(left_col);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title_lbl, p.title);
    lv_obj_set_width(title_lbl, lv_pct(100));
    lv_obj_set_style_text_font(title_lbl, theme::font_meta(), 0);
    lv_obj_set_style_text_color(title_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(title_lbl, 6, 0);

    if (p.email && p.email[0] != '\0') {
        lv_obj_t* email_lbl = lv_label_create(left_col);
        lv_label_set_long_mode(email_lbl, LV_LABEL_LONG_WRAP);
        lv_label_set_text(email_lbl, p.email);
        lv_obj_set_width(email_lbl, lv_pct(100));
        lv_obj_set_style_text_font(email_lbl, theme::font_meta(), 0);
        lv_obj_set_style_text_color(email_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(email_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(email_lbl, 20, 0);
    }

    if (p.phone && p.phone[0] != '\0') {
        lv_obj_t* phone_lbl = lv_label_create(left_col);
        lv_label_set_text(phone_lbl, p.phone);
        lv_obj_set_style_text_font(phone_lbl, theme::font_meta(), 0);
        lv_obj_set_style_text_color(phone_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(phone_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(phone_lbl, 8, 0);
    }

    lv_obj_t* spacer_bot = lv_obj_create(left_col);
    ui::clear_container(spacer_bot);
    lv_obj_set_size(spacer_bot, 0, 0);
    lv_obj_set_flex_grow(spacer_bot, 1);

    // ── Right column — photo, pinned to the same position as in calendar mode ──
    // Width = widget_profile_card::WIDTH (269 px) and horizontal padding = 8 px
    // each side match the right panel exactly, so the photo's visual centre
    // lands at the same X as in calendar mode.
    //
    // Vertical: read the live photo's absolute Y from the LVGL layout engine
    // (lv_obj_get_coords gives display-absolute coordinates). body_row and
    // right_col both start at y = STATUS_BAR_H (box pad_top), so:
    //   right_col pad_top = coords.y1 − STATUS_BAR_H
    lv_coord_t photo_pad_top = 0;
    if (ref_photo) {
        lv_area_t coords;
        lv_obj_get_coords(ref_photo, &coords);
        lv_coord_t offset = coords.y1 - STATUS_BAR_H;
        if (offset > 0) photo_pad_top = offset;
    }

    lv_obj_t* right_col = lv_obj_create(body_row);
    lv_obj_set_size(right_col, widget_profile_card::WIDTH, lv_pct(100));
    ui::clear_container(right_col);
    lv_obj_set_style_pad_left(right_col, 11, 0);
    lv_obj_set_style_pad_right(right_col, 5, 0);
    lv_obj_set_style_pad_top(right_col, photo_pad_top, 0);
    lv_obj_set_style_pad_bottom(right_col, 0, 0);
    lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* photo = lv_obj_create(right_col);
    lv_obj_set_size(photo, widget_profile_card::PHOTO_SIZE, widget_profile_card::PHOTO_SIZE);
    lv_obj_set_style_radius(photo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(photo, theme::color(0x2A3140), 0);
    lv_obj_set_style_bg_grad_color(photo, theme::color(0x1A1F28), 0);
    lv_obj_set_style_bg_grad_dir(photo, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(photo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(photo, theme::color(pres_color), 0);
    lv_obj_set_style_border_width(photo, 6, 0);
    lv_obj_set_style_shadow_color(photo, theme::color(pres_color), 0);
    lv_obj_set_style_shadow_width(photo, 40, 0);
    lv_obj_set_style_shadow_spread(photo, 8, 0);
    lv_obj_set_style_shadow_opa(photo,
        pres == widget_profile_card::Presence::Offline ? LV_OPA_TRANSP : LV_OPA_90, 0);
    lv_obj_set_style_pad_all(photo, 0, 0);
    lv_obj_clear_flag(photo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(photo, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* initials_lbl = lv_label_create(photo);
    lv_label_set_text(initials_lbl, p.initials);
    lv_obj_set_style_text_color(initials_lbl, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(initials_lbl, theme::font_large(), 0);
    lv_obj_center(initials_lbl);

    // 28 px gap between the body and the close button (LVGL has no margin style).
    lv_obj_t* gap = lv_obj_create(box);
    ui::clear_container(gap);
    lv_obj_set_size(gap, lv_pct(100), 28);

    lv_obj_t* close_btn = ui::make_btn(box, "Close", ui::BtnStyle::Tertiary,
                                       nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        lv_obj_del(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
    }, LV_EVENT_CLICKED, scrim);

    return scrim;
}

} // namespace modal_profile
