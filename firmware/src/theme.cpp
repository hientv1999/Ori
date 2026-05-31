#include "theme.h"

extern "C" {
extern const lv_font_t ori_font_hanken_20;
extern const lv_font_t ori_font_hanken_24;
extern const lv_font_t ori_font_hanken_26;
extern const lv_font_t ori_font_hanken_28;
extern const lv_font_t ori_font_hanken_30;
extern const lv_font_t ori_font_hanken_42;
extern const lv_font_t ori_font_hanken_48;
}

namespace theme {

const lv_font_t* font_body()    { return &ori_font_hanken_20; }
const lv_font_t* font_meta()    { return &ori_font_hanken_24; }
const lv_font_t* font_title()   { return &ori_font_hanken_26; }
const lv_font_t* font_h2()      { return &ori_font_hanken_28; }
const lv_font_t* font_time()    { return &ori_font_hanken_30; }
const lv_font_t* font_display() { return &ori_font_hanken_42; }
const lv_font_t* font_large()   { return &ori_font_hanken_48; }

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
