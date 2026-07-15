#include "widgets/widget_progress_ring.h"

#include "theme.h"

// Built on lv_arc.
//
//   - Background track: full circle, COLOR_DIVIDER at 8% opacity (matches
//     the prototype's rgba(255,255,255,0.08)).
//   - Indicator: arc from 12 o'clock (270°) clockwise, COLOR_ACCENT,
//     rounded ends to match the prototype's stroke-linecap: round.
//   - Two centered labels (big + small), absolutely positioned.

namespace {

struct RingState {
    lv_obj_t*   big_label;
    lv_obj_t*   small_label;
    int16_t     big_y_off;   // big-label vertical offset (≠0 when a sub-label shares the centre)
};

// LVGL arc helpers — values stored in degrees, NOT 0.1°.
constexpr uint16_t START_DEG_TOP = 270;  // 12 o'clock

uint16_t stroke_width_for(uint16_t size_px) {
    if (size_px < 140) return 6;
    if (size_px < 200) return 7;
    return 8;
}

} // namespace

namespace widget_progress_ring {

lv_obj_t* create(lv_obj_t* parent, uint16_t size_px,
                 int16_t x_offset, int16_t y_offset) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size_px, size_px);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

    const uint16_t stroke = stroke_width_for(size_px);

    // Track (background full circle).
    lv_obj_set_style_arc_color(arc, theme::color(0x1B1F26), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, stroke, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);

    // Indicator (progress).
    lv_obj_set_style_arc_color(arc, theme::color(theme::COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, stroke, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_x(arc, x_offset, 0);
    lv_obj_set_style_translate_y(arc, y_offset, 0);

    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, START_DEG_TOP);
    lv_arc_set_angles(arc, 0, 0);  // start at 0%

    auto* s = new RingState();
    s->big_y_off = 0;
    s->big_label = lv_label_create(arc);
    lv_label_set_text(s->big_label, "");
    lv_obj_set_style_text_color(s->big_label, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s->big_label, theme::font_large(), 0);
    lv_obj_set_style_text_align(s->big_label, LV_TEXT_ALIGN_CENTER, 0);
    // Default: perfectly centred. When a sub-label is later shown via
    // set_sub_label_text(), both labels are repositioned to share the space.
    lv_obj_align(s->big_label, LV_ALIGN_CENTER, 0, 0);

    s->small_label = lv_label_create(arc);
    lv_label_set_text(s->small_label, "");
    lv_obj_set_style_text_color(s->small_label, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s->small_label, theme::font_meta(), 0);
    lv_obj_align(s->small_label, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_flag(s->small_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_user_data(arc, s);
    lv_obj_add_event_cb(arc, [](lv_event_t* e) {
        delete static_cast<RingState*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, s);
    return arc;
}

void set_value(lv_obj_t* ring, uint8_t percent) {
    auto* s = static_cast<RingState*>(lv_obj_get_user_data(ring));
    if (!s) return;
    if (percent > 100) percent = 100;
    // 0..100 maps to 0..360 starting at the top (12 o'clock).
    uint16_t sweep = (uint16_t)((360 * (uint32_t)percent) / 100);
    lv_arc_set_angles(ring, 0, sweep);
}

void set_angle(lv_obj_t* ring, uint16_t degrees) {
    auto* s = static_cast<RingState*>(lv_obj_get_user_data(ring));
    if (!s) return;
    if (degrees > 360) degrees = 360;
    lv_arc_set_angles(ring, 0, degrees);
}

void set_label_text_center(lv_obj_t* ring, const char* s_text, int16_t x_offset) {
    auto* s = static_cast<RingState*>(lv_obj_get_user_data(ring));
    if (!s) return;
    if (s_text) {
        lv_label_set_text(s->big_label, s_text);
        lv_obj_clear_flag(s->big_label, LV_OBJ_FLAG_HIDDEN);
        // Honour big_y_off so the per-second time update doesn't re-pin the
        // label back to the exact centre (which sits low when a sub-label shares it).
        lv_obj_align(s->big_label,   LV_ALIGN_CENTER, x_offset, s->big_y_off);
    } else {
        lv_obj_add_flag(s->big_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_sub_label_text(lv_obj_t* ring, const char* s_text) {
    auto* s = static_cast<RingState*>(lv_obj_get_user_data(ring));
    if (!s) return;
    if (s_text) {
        lv_label_set_text(s->small_label, s_text);
        lv_obj_clear_flag(s->small_label, LV_OBJ_FLAG_HIDDEN);
        // Two-label layout: lift both so the combined block (big timer + sub-
        // label) is vertically centred in the ring instead of sitting ~16 px low.
        s->big_y_off = -16;
        lv_obj_align(s->big_label,   LV_ALIGN_CENTER, 0, s->big_y_off);
        lv_obj_align(s->small_label, LV_ALIGN_CENTER, 0, 24);
    } else {
        lv_obj_add_flag(s->small_label, LV_OBJ_FLAG_HIDDEN);
        // Single-label layout: restore big label to exact centre.
        s->big_y_off = 0;
        lv_obj_align(s->big_label, LV_ALIGN_CENTER, 0, s->big_y_off);
    }
}

void set_label_font(lv_obj_t* ring, const lv_font_t* font) {
    auto* s = static_cast<RingState*>(lv_obj_get_user_data(ring));
    if (!s || !font) return;
    lv_obj_set_style_text_font(s->big_label, font, 0);
}

} // namespace widget_progress_ring
