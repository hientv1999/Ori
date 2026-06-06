#include "screens/screen_no_meetings.h"

#include <lvgl.h>

#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// "No meetings today" empty state.
//
//   .empty {
//     centered, glyph 80x80 (COLOR_TEXT_TERTIARY),
//     headline 30px COLOR_TEXT_PRIMARY,
//     sub 18px COLOR_TEXT_TERTIARY
//   }
//
// LVGL has no native calendar glyph at this fidelity. We use an outlined
// rounded square as a placeholder — sufficient for the empty-state read
// at this size. Real icon font lands in M8.

namespace {

lv_obj_t* make_cal_glyph(lv_obj_t* parent) {
    // Matches the HTML prototype's `#i-cal` SVG symbol (viewBox 24x24,
    // rendered at 80x80 → scale ×3.33). Three pieces:
    //   1. Hollow rounded body rect            — SVG rect x=3,y=5,w=18,h=16,rx=2
    //   2. Thin horizontal divider near the top — SVG path M3 9 h18
    //   3. Two short vertical tabs (rings)      — SVG paths M8 3v4 / M16 3v4

    // Outer 80x80 positioning frame — transparent, no border.
    lv_obj_t* glyph = lv_obj_create(parent);
    lv_obj_set_size(glyph, 80, 80);
    ui::clear_container(glyph);

    // 1) Body — hollow rounded rect. (3,5,18,16,rx=2) → (10,17,60,53,rx=7).
    lv_obj_t* body = lv_obj_create(glyph);
    lv_obj_set_size(body, 60, 53);
    lv_obj_set_pos(body, 10, 17);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(body, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_border_width(body, 3, 0);
    lv_obj_set_style_radius(body, 7, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 2) Header divider — thin horizontal line at y=9 SVG → y=30 absolute,
    // spanning the body's full width (overlaps the side borders, like the SVG).
    lv_obj_t* divider = lv_obj_create(glyph);
    lv_obj_set_size(divider, 60, 3);
    lv_obj_set_pos(divider, 10, 30);
    lv_obj_set_style_bg_color(divider, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    // 3) Two top tabs (rings) — vertical lines straddling the body's top edge.
    // SVG x=8 / x=16, y=3..7 → centred at absolute x=27 / x=53, y=10..24 (14 px tall).
    auto make_tab = [&](int16_t cx) {
        lv_obj_t* tab = lv_obj_create(glyph);
        lv_obj_set_size(tab, 4, 14);
        lv_obj_set_pos(tab, cx - 2, 10);
        lv_obj_set_style_bg_color(tab, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_radius(tab, 2, 0);
        lv_obj_set_style_pad_all(tab, 0, 0);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_CLICKABLE);
    };
    make_tab(27);
    make_tab(53);

    return glyph;
}

} // namespace

namespace screen_no_meetings {

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    widget_status_bar::create(screen);

    lv_obj_t* body = ui::make_screen_body(screen);

    // Left panel — centered glyph + headline + subtitle.
    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, 528, lv_pct(100));
    ui::clear_container(left);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* glyph = make_cal_glyph(left);
    lv_obj_set_style_pad_bottom(glyph, 0, 0);

    lv_obj_t* headline = lv_label_create(left);
    lv_label_set_text_static(headline, "No meetings today");
    lv_obj_set_style_text_color(headline, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(headline, theme::font_display(), 0);
    lv_obj_set_style_pad_top(headline, 22, 0);

    lv_obj_t* sub = lv_label_create(left);
    lv_label_set_text_static(sub, "Enjoy the focus time");
    lv_obj_set_style_text_color(sub, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(sub, theme::font_meta(), 0);
    lv_obj_set_style_pad_top(sub, 10, 0);

    ui::make_panel_divider(body);
    widget_profile_card::create(body);
    return screen;
}

} // namespace screen_no_meetings
