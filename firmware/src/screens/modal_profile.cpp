#include "screens/modal_profile.h"

#include <lvgl.h>

#include "photo_cache.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"  // for WIDTH, PHOTO_SIZE, get_default_presence

namespace modal_profile {

// Status-bar height — body row (and right panel) start at this Y offset.
static constexpr lv_coord_t STATUS_BAR_H = 84;

lv_obj_t* create(lv_obj_t* base_screen, lv_obj_t* ref_photo) {
    const char* p_name  = widget_profile_card::get_profile_name();
    if (!p_name  || !p_name[0])  p_name  = "\xe2\x80\x94";
    const char* p_title = widget_profile_card::get_profile_title();
    if (!p_title || !p_title[0]) p_title = "\xe2\x80\x94";
    const char* p_email = widget_profile_card::get_profile_email();
    const char* p_phone = widget_profile_card::get_profile_phone();

    // Gradient ring colours — same as the live profile card ring.
    widget_profile_card::Presence pres = widget_profile_card::get_default_presence();
    uint32_t pres_color, pres_color_light, pres_color_dark;
    switch (pres) {
        case widget_profile_card::Presence::Available:
            pres_color       = theme::COLOR_PRESENCE_AVAILABLE;
            pres_color_light = theme::COLOR_PRESENCE_AVAILABLE_LIGHT;
            pres_color_dark  = theme::COLOR_PRESENCE_AVAILABLE_DARK;  break;
        case widget_profile_card::Presence::Busy:
            pres_color       = theme::COLOR_PRESENCE_BUSY;
            pres_color_light = theme::COLOR_PRESENCE_BUSY_LIGHT;
            pres_color_dark  = theme::COLOR_PRESENCE_BUSY_DARK;       break;
        case widget_profile_card::Presence::Away:
            pres_color       = theme::COLOR_PRESENCE_AWAY;
            pres_color_light = theme::COLOR_PRESENCE_AWAY_LIGHT;
            pres_color_dark  = theme::COLOR_PRESENCE_AWAY_DARK;       break;
        default:
            pres_color       = theme::COLOR_PRESENCE_OFFLINE;
            pres_color_light = theme::COLOR_PRESENCE_OFFLINE_LIGHT;
            pres_color_dark  = theme::COLOR_PRESENCE_OFFLINE_DARK;    break;
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
    lv_label_set_text(name_lbl, p_name);
    lv_obj_set_width(name_lbl, lv_pct(100));
    lv_obj_set_style_text_font(name_lbl, theme::font_h2(), 0);
    lv_obj_set_style_text_color(name_lbl, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* title_lbl = lv_label_create(left_col);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title_lbl, p_title);
    lv_obj_set_width(title_lbl, lv_pct(100));
    lv_obj_set_style_text_font(title_lbl, theme::font_meta(), 0);
    lv_obj_set_style_text_color(title_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(title_lbl, 6, 0);

    lv_obj_t* email_lbl  = nullptr;
    lv_obj_t* phone_lbl  = nullptr;
    lv_obj_t* status_lbl = nullptr;

    if (p_email && p_email[0] != '\0') {
        email_lbl = lv_label_create(left_col);
        lv_label_set_long_mode(email_lbl, LV_LABEL_LONG_WRAP);
        lv_label_set_text(email_lbl, p_email);
        lv_obj_set_width(email_lbl, lv_pct(100));
        lv_obj_set_style_text_font(email_lbl, theme::font_meta(), 0);
        lv_obj_set_style_text_color(email_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(email_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(email_lbl, 20, 0);
    }

    if (p_phone && p_phone[0] != '\0') {
        phone_lbl = lv_label_create(left_col);
        lv_label_set_text(phone_lbl, p_phone);
        lv_obj_set_style_text_font(phone_lbl, theme::font_meta(), 0);
        lv_obj_set_style_text_color(phone_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(phone_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(phone_lbl, 8, 0);
    }

    // Status line — presence colour matches the profile photo border.
    {
        const char* status_str;
        switch (pres) {
            case widget_profile_card::Presence::Available: status_str = "Status: Available"; break;
            case widget_profile_card::Presence::Busy:      status_str = "Status: Busy";      break;
            case widget_profile_card::Presence::Away:      status_str = "Status: Away";       break;
            default:                                       status_str = "Status: Offline";    break;
        }
        status_lbl = lv_label_create(left_col);
        lv_label_set_text(status_lbl, status_str);
        lv_obj_set_style_text_font(status_lbl, theme::font_meta(), 0);
        lv_obj_set_style_text_color(status_lbl, theme::color(pres_color), 0);
        lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(status_lbl, 24, 0);
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

    // Gradient presence ring — matches the profile-card ring structure exactly.
    // Registered as g_modal_photo so set_default_presence() can push gradient
    // updates to it live while the overlay is open.
    lv_obj_t* photo_ring = lv_obj_create(right_col);
    lv_obj_set_size(photo_ring,
        widget_profile_card::PHOTO_SIZE + 12,
        widget_profile_card::PHOTO_SIZE + 12);
    lv_obj_set_style_radius(photo_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(photo_ring, theme::color(pres_color_light), 0);
    lv_obj_set_style_bg_grad_color(photo_ring, theme::color(pres_color_dark), 0);
    lv_obj_set_style_bg_grad_dir(photo_ring, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(photo_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(photo_ring, 0, 0);
    lv_obj_set_style_pad_all(photo_ring, 0, 0);
    lv_obj_clear_flag(photo_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(photo_ring, LV_OBJ_FLAG_CLICKABLE);
    widget_profile_card::register_modal_photo(photo_ring);

    // Inner photo circle — the ring peeks 6 px around all edges.
    lv_obj_t* photo = lv_obj_create(photo_ring);
    lv_obj_set_size(photo, widget_profile_card::PHOTO_SIZE, widget_profile_card::PHOTO_SIZE);
    lv_obj_center(photo);
    lv_obj_set_style_radius(photo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(photo, theme::color(0x2A3140), 0);
    lv_obj_set_style_bg_grad_color(photo, theme::color(0x1A1F28), 0);
    lv_obj_set_style_bg_grad_dir(photo, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(photo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(photo, 0, 0);
    lv_obj_set_style_pad_all(photo, 0, 0);
    lv_obj_clear_flag(photo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(photo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(photo, true, 0);

    widget_profile_card::ModalLabels modal_labels = {};
    modal_labels.status_lbl = status_lbl;
    modal_labels.name_lbl   = name_lbl;
    modal_labels.title_lbl  = title_lbl;
    modal_labels.email_lbl  = email_lbl;
    modal_labels.phone_lbl  = phone_lbl;
    widget_profile_card::register_modal_labels(modal_labels);

    const lv_image_dsc_t* photo_dsc = photo_cache::get();
    if (!photo_dsc) photo_dsc = photo_cache::get_profile_placeholder();
    if (photo_dsc) {
        lv_obj_t* img = lv_image_create(photo);
        lv_image_set_src(img, photo_dsc);
        lv_obj_set_size(img, widget_profile_card::PHOTO_SIZE, widget_profile_card::PHOTO_SIZE);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    }

    // 28 px gap between the body and the close button (LVGL has no margin style).
    lv_obj_t* gap = lv_obj_create(box);
    ui::clear_container(gap);
    lv_obj_set_size(gap, lv_pct(100), 28);

    lv_obj_t* close_btn = ui::make_btn(box, "Close", ui::BtnStyle::Tertiary,
                                       nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        lv_obj_delete(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
    }, LV_EVENT_CLICKED, scrim);
    lv_obj_add_event_cb(scrim, [](lv_event_t*) {
        widget_profile_card::unregister_modal_photo();
        widget_profile_card::unregister_modal_labels();
    }, LV_EVENT_DELETE, nullptr);

    return scrim;
}

} // namespace modal_profile
