#include "theme.h"

#include "emoji_font.h"

extern "C" {
extern const lv_font_t ori_font_hanken_20;
extern const lv_font_t ori_font_hanken_24;
extern const lv_font_t ori_font_hanken_26;
extern const lv_font_t ori_font_hanken_28;
extern const lv_font_t ori_font_hanken_30;
extern const lv_font_t ori_font_hanken_42;
extern const lv_font_t ori_font_hanken_48;
extern const lv_font_t ori_font_hanken_96;
extern const lv_font_t ori_font_hanken_90;
}

namespace theme {

// Emoji-fallback wrappers. The generated Hanken fonts are const (flash), so we
// keep RAM copies with .fallback pointing at the color-emoji imgfont. Populated
// by init_emoji_fallback() at boot; until then the getters return the plain
// fonts. Only fonts that carry user/notification text get emoji support.
namespace {
lv_font_t s_body, s_meta, s_title, s_h2;
bool      s_emoji_ready = false;
}

void init_emoji_fallback() {
    const lv_font_t* ef = emoji_font::get();
    if (!ef) return;  // empty emoji set / creation failed → keep plain fonts
    s_body  = ori_font_hanken_20; s_body.fallback  = ef;
    s_meta  = ori_font_hanken_24; s_meta.fallback  = ef;
    s_title = ori_font_hanken_26; s_title.fallback = ef;
    s_h2    = ori_font_hanken_28; s_h2.fallback    = ef;
    s_emoji_ready = true;
}

const lv_font_t* font_body()    { return s_emoji_ready ? &s_body  : &ori_font_hanken_20; }
const lv_font_t* font_meta()    { return s_emoji_ready ? &s_meta  : &ori_font_hanken_24; }
const lv_font_t* font_title()   { return s_emoji_ready ? &s_title : &ori_font_hanken_26; }
const lv_font_t* font_h2()      { return s_emoji_ready ? &s_h2    : &ori_font_hanken_28; }
const lv_font_t* font_time()    { return &ori_font_hanken_30; }
const lv_font_t* font_display() { return &ori_font_hanken_42; }
const lv_font_t* font_large()   { return &ori_font_hanken_48; }
const lv_font_t* font_clock_xl() { return &ori_font_hanken_96; }
const lv_font_t* font_wordmark_xl() { return &ori_font_hanken_90; }

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
