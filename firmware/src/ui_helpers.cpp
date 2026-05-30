#include "ui_helpers.h"

#include "theme.h"
#include "widgets/widget_status_bar.h"


namespace ui {

lv_obj_t* make_screen_body(lv_obj_t* screen) {
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_set_size(body, 800, 480 - widget_status_bar::HEIGHT);
    lv_obj_set_pos(body, 0, widget_status_bar::HEIGHT);
    lv_obj_set_style_bg_color(body, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // LVGL's default pad_column on lv_obj_create() is non-zero, which
    // adds ~11 px between panels and shoves the profile card off screen.
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_set_style_pad_column(body, 0, 0);
    return body;
}

lv_obj_t* make_panel_divider(lv_obj_t* parent) {
    lv_obj_t* div = lv_obj_create(parent);
    lv_obj_set_size(div, 5, lv_pct(100));
    lv_obj_set_style_bg_color(div, theme::color(theme::COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_pad_all(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);
    return div;
}

static void btn_glow_anim_cb(void* obj, int32_t v) {
    lv_obj_set_style_shadow_opa(static_cast<lv_obj_t*>(obj), (lv_opa_t)v, 0);
}

lv_obj_t* make_btn(lv_obj_t* parent, const char* text,
                   BtnStyle style,
                   lv_event_cb_t cb, void* user,
                   int16_t pad_v, int16_t pad_h,
                   const lv_font_t* font) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(btn, pad_h, 0);
    lv_obj_set_style_pad_right(btn, pad_h, 0);
    lv_obj_set_style_pad_top(btn, pad_v, 0);
    lv_obj_set_style_pad_bottom(btn, pad_v, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);

    // Shared ghost-button emphasis so every button inherits a visible outline.
    lv_obj_set_style_border_width(btn, 4, 0);
    lv_obj_set_style_shadow_width(btn, 28, 0);
    lv_obj_set_style_shadow_spread(btn, 4, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_x(btn, 0, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 0, 0);
    lv_obj_set_style_opa(btn, LV_OPA_60, LV_STATE_PRESSED);

    uint32_t text_color = theme::COLOR_TEXT_SECONDARY;

    switch (style) {
        case BtnStyle::Primary:
            lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_ACCENT), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_10, 0);
            lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_ACCENT), 0);
            lv_obj_set_style_border_opa(btn, LV_OPA_70, 0);
            lv_obj_set_style_shadow_color(btn, theme::color(theme::COLOR_ACCENT), 0);
            lv_obj_set_style_shadow_width(btn, 44, 0);
            lv_obj_set_style_shadow_opa(btn, LV_OPA_10, 0);
            lv_obj_set_style_shadow_spread(btn, 6, 0);
            text_color = theme::COLOR_ACCENT;
            break;
        case BtnStyle::Danger:
            lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_DANGER), 0);
            lv_obj_set_style_shadow_color(btn, theme::color(theme::COLOR_DANGER), 0);
            lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
            text_color = theme::COLOR_DANGER;
            break;
        case BtnStyle::Secondary:
            lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_DIVIDER_STRONG), 0);
            lv_obj_set_style_shadow_color(btn, theme::color(theme::COLOR_DIVIDER_STRONG), 0);
            lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
            text_color = theme::COLOR_TEXT_SECONDARY;
            break;
        case BtnStyle::Tertiary:
            lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_DIVIDER), 0);
            lv_obj_set_style_border_opa(btn, LV_OPA_60, 0);
            // Tertiary keeps the thick ghost border but no glow.
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_shadow_spread(btn, 0, 0);
            lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
            text_color = theme::COLOR_TEXT_TERTIARY;
            break;
    }

    // Breathing glow for all styles except Tertiary — pulses shadow_opa via
    // lv_anim (not LVGL style transitions, which cause tearing on the
    // bounce-buffer display). lv_anim fires inside lv_task_handler() so one
    // invalidation per flush; no horizontal tear bands.
    if (style != BtnStyle::Tertiary) {
        const lv_opa_t glow_max = (style == BtnStyle::Primary) ? LV_OPA_90 :
                                  (style == BtnStyle::Danger)   ? LV_OPA_70 : LV_OPA_50;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, btn);
        lv_anim_set_exec_cb(&a, btn_glow_anim_cb);
        lv_anim_set_values(&a, LV_OPA_10, (int32_t)glow_max);
        lv_anim_set_time(&a, 1200);
        lv_anim_set_playback_time(&a, 1200);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            lv_anim_delete(lv_event_get_current_target(e), btn_glow_anim_cb);
        }, LV_EVENT_DELETE, nullptr);
    }

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, theme::color(text_color), 0);
    lv_obj_set_style_text_font(lbl, font ? font : theme::font_meta(), 0);
    if (style == BtnStyle::Primary) {
        lv_obj_set_style_text_letter_space(lbl, 4, 0);
    }
    lv_obj_center(lbl);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }
    return btn;
}

} // namespace ui
