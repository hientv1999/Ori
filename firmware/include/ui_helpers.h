#pragma once

#include <lvgl.h>

// Layout helpers shared across all screen and widget files.
//
// Three patterns repeat identically throughout the codebase and are
// captured here to keep callsites readable:
//
//  ui::clear_container(obj)   — strips transparent-bg, zero-border,
//                               zero-padding, and disables scroll+click
//                               on a layout-only container object.
//
//  ui::make_screen_body(s)    — creates the 800×396 flex-row body
//                               positioned below the status bar, with
//                               COLOR_BG fill and zero row/column gaps.
//                               Used by every two-panel runtime screen.
//
//  ui::make_panel_divider(p)  — creates the 5 px × 100% vertical
//                               divider that separates the left panel
//                               from the profile card.

namespace ui {

struct ModalLayout {
    lv_obj_t* scrim;
    lv_obj_t* card;
    lv_obj_t* scroll_area;
    lv_obj_t* actions;
};

// Shared ghost-button hierarchy used across setup screens and modals.
enum class BtnStyle {
    Primary,    // accent border/text + stronger glow
    Danger,     // danger border/text + danger glow
    Tertiary,   // faint border/text, no glow
};

inline void clear_container(lv_obj_t* obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* make_screen_body(lv_obj_t* screen);
lv_obj_t* make_panel_divider(lv_obj_t* parent);
ModalLayout make_modal_layout(lv_obj_t* base_screen,
                              lv_coord_t card_w = 520,
                              lv_coord_t card_h = 400);
lv_obj_t* make_btn(lv_obj_t* parent, const char* text,
                   BtnStyle style,
                   lv_event_cb_t cb = nullptr, void* user = nullptr,
                   int16_t pad_v = 14, int16_t pad_h = 28,
                   const lv_font_t* font = nullptr);

} // namespace ui
