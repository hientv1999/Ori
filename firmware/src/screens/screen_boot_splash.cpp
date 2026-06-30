#include "screens/screen_boot_splash.h"

#include "theme.h"
#include "ui_helpers.h"

namespace {

// 3x ui::make_brand_mark() — boot-splash-only, so the setup flow's wordmark
// (still 1x via ui::make_brand_mark()) is untouched. Needs its own font:
// LVGL fonts are fixed-size bitmap assets, not arbitrarily scalable, so "3x"
// means a real 90 px font (theme::font_wordmark_xl(), 3x the 30 px
// theme::font_time() the 1x mark uses) rather than a transform-scaled 30 px
// render — keeps the splash crisp instead of blurry. That font is subset to
// just 'o'/'r'/'i'/space (the only glyphs this string ever needs) to keep
// its flash footprint small.
lv_obj_t* make_brand_mark_xl(lv_obj_t* parent) {
    // Accent at 58% over pure black — same constant as ui::make_brand_mark().
    constexpr uint32_t ACCENT_58 = 0x826B3D;

    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_SIZE_CONTENT, 108);   // 3x the 1x mark's 36 px
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_column(root, 30, 0);      // 3x 10
    lv_obj_set_style_margin_top(root, -6, 0);      // 3x -2
    lv_obj_set_style_margin_bottom(root, 12, 0);   // 3x 4
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* line_l = lv_obj_create(root);
    lv_obj_set_size(line_l, 132, 6);               // 3x 44x2
    lv_obj_set_style_radius(line_l, 3, 0);
    lv_obj_set_style_bg_color(line_l, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_grad_color(line_l, theme::color(ACCENT_58), 0);
    lv_obj_set_style_bg_grad_dir(line_l, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(line_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line_l, 0, 0);
    lv_obj_set_style_pad_all(line_l, 0, 0);
    lv_obj_clear_flag(line_l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line_l, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* word = lv_label_create(root);
    lv_label_set_text(word, "o#E0B86A r#i");
    lv_label_set_recolor(word, true);
    lv_obj_set_style_text_color(word, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(word, theme::font_wordmark_xl(), 0);
    lv_obj_set_style_text_letter_space(word, 30, 0);   // 3x 10

    lv_obj_t* line_r = lv_obj_create(root);
    lv_obj_set_size(line_r, 132, 6);
    lv_obj_set_style_radius(line_r, 3, 0);
    lv_obj_set_style_bg_color(line_r, theme::color(ACCENT_58), 0);
    lv_obj_set_style_bg_grad_color(line_r, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_grad_dir(line_r, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(line_r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line_r, 0, 0);
    lv_obj_set_style_pad_all(line_r, 0, 0);
    lv_obj_clear_flag(line_r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line_r, LV_OBJ_FLAG_CLICKABLE);

    return root;
}

} // namespace

namespace screen_boot_splash {

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, 800, 480);
    lv_obj_set_style_bg_color(screen, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* mark = make_brand_mark_xl(screen);
    lv_obj_center(mark);

    return screen;
}

} // namespace screen_boot_splash
