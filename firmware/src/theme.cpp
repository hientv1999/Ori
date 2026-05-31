#include "theme.h"

// Ori-branded Montserrat fonts live in src/fonts/. They include the standard
// ASCII range PLUS U+B0 (°), U+B7 (·), U+2014 (—), U+2022 (•), U+2026 (…).
// LVGL's bundled Montserrat is ASCII-only and was missing these glyphs,
// which rendered as empty boxes throughout the prototype's status bar,
// meeting times, ellipses, and PTO date ranges. See lv_conf.h's
// LV_FONT_CUSTOM_DECLARE block for the declarations.
extern "C" {
extern const lv_font_t ori_font_montserrat_16;
extern const lv_font_t ori_font_montserrat_20;
extern const lv_font_t ori_font_montserrat_22;
extern const lv_font_t ori_font_montserrat_24;
extern const lv_font_t ori_font_montserrat_28;
extern const lv_font_t ori_font_montserrat_30;
extern const lv_font_t ori_font_montserrat_36;
extern const lv_font_t ori_font_montserrat_42;
extern const lv_font_t ori_font_montserrat_48;
}

namespace theme {

const lv_font_t* font_small()   { return &ori_font_montserrat_16; }
const lv_font_t* font_body()    { return &ori_font_montserrat_20; }
const lv_font_t* font_meta()    { return &ori_font_montserrat_22; }
const lv_font_t* font_title()   { return &ori_font_montserrat_24; }
const lv_font_t* font_h2()      { return &ori_font_montserrat_28; }
const lv_font_t* font_time()    { return &ori_font_montserrat_30; }
const lv_font_t* font_display() { return &ori_font_montserrat_42; }
const lv_font_t* font_large()   { return &ori_font_montserrat_48; }

void apply_to_screen(lv_obj_t* screen) {
    lv_obj_set_style_bg_color(screen, color(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, color(COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_font(screen, font_meta(), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

} // namespace theme
