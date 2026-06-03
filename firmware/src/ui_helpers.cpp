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

ModalLayout make_modal_layout(lv_obj_t* base_screen, lv_coord_t card_w, lv_coord_t card_h) {
    ModalLayout layout{};

    layout.scrim = lv_obj_create(base_screen);
    lv_obj_set_size(layout.scrim, 800, 480);
    lv_obj_set_pos(layout.scrim, 0, 0);
    lv_obj_set_style_bg_color(layout.scrim, theme::color(theme::COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(layout.scrim, theme::SCRIM_OPA, 0);
    lv_obj_set_style_border_width(layout.scrim, 0, 0);
    lv_obj_set_style_pad_all(layout.scrim, 0, 0);
    lv_obj_clear_flag(layout.scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(layout.scrim, LV_OBJ_FLAG_CLICKABLE);

    layout.card = lv_obj_create(layout.scrim);
    lv_obj_set_size(layout.card, card_w, card_h);
    lv_obj_center(layout.card);
    lv_obj_set_style_bg_color(layout.card, theme::color(theme::COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(layout.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(layout.card, theme::color(theme::COLOR_DIVIDER_STRONG), 0);
    lv_obj_set_style_border_width(layout.card, 1, 0);
    lv_obj_set_style_radius(layout.card, 18, 0);
    lv_obj_set_style_shadow_color(layout.card, theme::color(0x000000), 0);
    lv_obj_set_style_shadow_width(layout.card, 30, 0);
    lv_obj_set_style_shadow_opa(layout.card, LV_OPA_70, 0);
    lv_obj_set_style_pad_left(layout.card, 32, 0);
    lv_obj_set_style_pad_right(layout.card, 32, 0);
    lv_obj_set_style_pad_top(layout.card, 32, 0);
    lv_obj_set_style_pad_bottom(layout.card, 32, 0);
    lv_obj_clear_flag(layout.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(layout.card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(layout.card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    layout.scroll_area = lv_obj_create(layout.card);
    lv_obj_set_width(layout.scroll_area, lv_pct(100));
    lv_obj_set_flex_grow(layout.scroll_area, 1);
    lv_obj_set_style_bg_opa(layout.scroll_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layout.scroll_area, 0, 0);
    lv_obj_set_style_pad_all(layout.scroll_area, 0, 0);
    lv_obj_add_flag(layout.scroll_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(layout.scroll_area, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(layout.scroll_area, LV_DIR_VER);
    lv_obj_set_flex_flow(layout.scroll_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(layout.scroll_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    layout.actions = lv_obj_create(layout.card);
    lv_obj_set_size(layout.actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(layout.actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layout.actions, 0, 0);
    lv_obj_set_style_pad_left(layout.actions, 8, 0);
    lv_obj_set_style_pad_right(layout.actions, 8, 0);
    lv_obj_set_style_pad_bottom(layout.actions, 8, 0);
    lv_obj_set_style_pad_top(layout.actions, 8, 0);
    lv_obj_set_style_pad_column(layout.actions, 16, 0);
    lv_obj_clear_flag(layout.actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(layout.actions, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(layout.actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(layout.actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    return layout;
}

static void btn_glow_anim_cb(void* obj, int32_t v) {
    lv_obj_set_style_shadow_opa(static_cast<lv_obj_t*>(obj), (lv_opa_t)v, 0);
}

lv_obj_t* make_btn(lv_obj_t* parent, const char* text,
                   BtnStyle style,
                   lv_event_cb_t cb, void* user,
                   int16_t pad_v, int16_t pad_h,
                   const lv_font_t* font) {
    const lv_font_t* active_font = font ? font : theme::font_meta();
    const lv_coord_t font_h = lv_font_get_line_height(active_font);
    const lv_coord_t glow_w = font_h;
    const lv_coord_t glow_spread = font_h/4;

    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(btn, pad_h, 0);
    lv_obj_set_style_pad_right(btn, pad_h, 0);
    lv_obj_set_style_pad_top(btn, pad_v, 0);
    lv_obj_set_style_pad_bottom(btn, pad_v, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);

    // Shared ghost-button emphasis so every button inherits a visible outline.
    lv_obj_set_style_border_width(btn, glow_spread/4, 0);
    lv_obj_set_style_shadow_width(btn, glow_w, 0);
    lv_obj_set_style_shadow_spread(btn, glow_spread, 0);
    // Danger glow profile is the baseline for non-tertiary buttons.
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_shadow_ofs_x(btn, 0, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 0, 0);
    lv_obj_set_style_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_90, 0);
    lv_color_t text_color = theme::color(theme::COLOR_TEXT_SECONDARY);

    switch (style) {
        case BtnStyle::Primary:
            text_color = theme::color(theme::COLOR_ACCENT);
            break;

        case BtnStyle::Danger:
            text_color = theme::color(theme::COLOR_DANGER);
            break;

        case BtnStyle::Tertiary:
            // Tertiary has no glow.
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_shadow_spread(btn, 0, 0);
            break;
    }

    lv_obj_set_style_border_color(btn, text_color, 0);
    lv_obj_set_style_shadow_color(btn, text_color, 0);

    // Breathing glow for all styles except Tertiary — pulses shadow_opa via
    // lv_anim (not LVGL style transitions, which cause tearing on the
    // bounce-buffer display). lv_anim fires inside lv_task_handler() so one
    // invalidation per flush; no horizontal tear bands.
    if (style != BtnStyle::Tertiary) {
        const lv_opa_t glow_max = LV_OPA_70;
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
    lv_obj_set_style_text_color(lbl, text_color, 0);
    lv_obj_set_style_text_font(lbl, active_font, 0);
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
