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

// Circular close affordance pinned to the top-right corner of a modal `card`.
// The "X" is drawn from two crossing lines (no symbol-font dependency). Ignores
// the card's flex layout. `cb` fires on tap (typically deletes the scrim).
// Returns the button. Use instead of a bottom "Close" button on overlays.
lv_obj_t* add_close_x(lv_obj_t* card, lv_event_cb_t cb = nullptr, void* user = nullptr);

// Copy `in` to `out`, dropping every character the UI font (Hanken) can't
// render — emoji, CJK, and any script outside the font's Latin repertoire —
// and tidying the whitespace the drops leave behind (collapses runs of spaces,
// trims line ends; newlines preserved as LVGL line breaks). Glyph presence is
// queried against the live font, so broadening the font subset automatically
// widens what survives. Always NUL-terminates; safe if in == nullptr (→ "").
// Apply at text ingress (BLE/ANCS handlers) so stored strings are render-clean.
// Returns out.
const char* sanitize_text(const char* in, char* out, size_t out_sz);

} // namespace ui
