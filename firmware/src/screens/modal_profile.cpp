#include "screens/modal_profile.h"

#include <lvgl.h>

#include "photo_cache.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"

namespace modal_profile {

lv_obj_t* create(lv_obj_t* base_screen, lv_obj_t* ref_photo) {
    const char* p_name  = widget_profile_card::get_profile_name();
    if (!p_name  || !p_name[0])  p_name  = "No name";
    const char* p_title = widget_profile_card::get_profile_title();
    if (!p_title || !p_title[0]) p_title = "No position";
    const char* p_email = widget_profile_card::get_profile_email();
    const char* p_phone = widget_profile_card::get_profile_phone();

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

    // Independent screen — pure black background, no status bar.
    // Tap the profile photo to return to base_screen.
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, theme::color(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Outer box — full 800×480 screen, no padding (this is a standalone screen
    // with no status bar). body_row fills all 480 px so LV_FLEX_ALIGN_CENTER
    // in left_col centres the text block exactly at screen midpoint (y=240).
    lv_obj_t* box = lv_obj_create(screen);
    lv_obj_set_size(box, 800, 480);
    lv_obj_set_pos(box, 0, 0);
    ui::clear_container(box);
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

    // ── Left column ───────────────────────────────────────────────────────────
    // Explicit height = full screen height so LVGL doesn't need to resolve it
    // through two layers of flex growth. LV_FLEX_ALIGN_CENTER on the main axis
    // centres the content block at y=240 (screen midpoint).
    lv_obj_t* left_col = lv_obj_create(body_row);
    lv_obj_set_flex_grow(left_col, 1);
    lv_obj_set_height(left_col, 480);
    lv_obj_set_style_bg_opa(left_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_col, 0, 0);
    lv_obj_set_style_pad_left(left_col, 40, 0);
    lv_obj_set_style_pad_right(left_col, 20, 0);
    lv_obj_set_style_pad_top(left_col, 0, 0);
    lv_obj_set_style_pad_bottom(left_col, 0, 0);
    lv_obj_clear_flag(left_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

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

    lv_obj_t* status_lbl = nullptr;

    lv_obj_t* email_lbl = lv_label_create(left_col);
    lv_label_set_long_mode(email_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(email_lbl, (p_email && p_email[0]) ? p_email : "No email");
    lv_obj_set_width(email_lbl, lv_pct(100));
    lv_obj_set_style_text_font(email_lbl, theme::font_meta(), 0);
    lv_obj_set_style_text_color(email_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(email_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(email_lbl, 20, 0);

    lv_obj_t* phone_lbl = lv_label_create(left_col);
    lv_label_set_text(phone_lbl, (p_phone && p_phone[0]) ? p_phone : "No phone number");
    lv_obj_set_style_text_font(phone_lbl, theme::font_meta(), 0);
    lv_obj_set_style_text_color(phone_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(phone_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(phone_lbl, 8, 0);

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

    // ── Right column — photo pinned to the same absolute Y as in calendar mode ─
    // body_row now starts at y=0 (box has no pad_top), so photo_pad_top = coords.y1
    // places the ring at the same absolute screen position as the live card.
    lv_coord_t photo_pad_top = 0;
    if (ref_photo) {
        lv_area_t coords;
        lv_obj_get_coords(ref_photo, &coords);
        if (coords.y1 > 0) photo_pad_top = coords.y1;
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

    lv_obj_t* photo_ring = lv_obj_create(right_col);
    lv_obj_set_size(photo_ring,
        widget_profile_card::PHOTO_SIZE + 24,
        widget_profile_card::PHOTO_SIZE + 24);
    lv_obj_set_style_radius(photo_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(photo_ring, theme::color(pres_color_light), 0);
    lv_obj_set_style_bg_grad_color(photo_ring, theme::color(pres_color_dark), 0);
    lv_obj_set_style_bg_grad_dir(photo_ring, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(photo_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(photo_ring, 0, 0);
    lv_obj_set_style_pad_all(photo_ring, 0, 0);
    lv_obj_clear_flag(photo_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(photo_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(photo_ring, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(photo_ring, [](lv_event_t* e) {
        lv_obj_t* prev = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
        lv_scr_load_anim(prev, LV_SCR_LOAD_ANIM_NONE, 0, 0, /*auto_del=*/true);
    }, LV_EVENT_CLICKED, base_screen);
    widget_profile_card::register_modal_photo(photo_ring);

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

    // Always create the image (even with no photo yet) and register it so
    // set_photo() can update it live when a photo arrives over BLE while the
    // modal is open. Falls back to the placeholder, matching the base card.
    lv_obj_t* img = lv_image_create(photo);
    lv_obj_set_size(img, widget_profile_card::PHOTO_SIZE, widget_profile_card::PHOTO_SIZE);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    const lv_image_dsc_t* photo_dsc = photo_cache::get();
    if (!photo_dsc) photo_dsc = photo_cache::get_profile_placeholder();
    if (photo_dsc) {
        lv_image_set_src(img, photo_dsc);
    } else {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
    widget_profile_card::register_modal_photo_img(img);

    lv_obj_add_event_cb(screen, [](lv_event_t*) {
        widget_profile_card::unregister_modal_photo();
        widget_profile_card::unregister_modal_photo_img();
        widget_profile_card::unregister_modal_labels();
    }, LV_EVENT_DELETE, nullptr);

    // Load as independent screen; keep base_screen alive for the go-back tap.
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, /*auto_del=*/false);

    return screen;
}

} // namespace modal_profile
