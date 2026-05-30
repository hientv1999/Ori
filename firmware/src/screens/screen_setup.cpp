#include "screens/screen_setup.h"

#include <Arduino.h>
#include <lvgl.h>

#include "mock_data.h"
#include "state_machine.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_progress_ring.h"

// Setup wizard — single LVGL screen, six sub-states. Status bar is OFF
// across the entire flow.
//
// Layout invariant (setup-flow.md):
//   - Step dots are anchored at y = 440 (i.e. 40 px above the bottom of the
//     480 px screen). They MUST NOT move between pages.
//   - On the Complete sub-state, the dot row is hidden (the page itself is
//     a brief acknowledgement before the wizard exits).

namespace {

constexpr int16_t DOT_ROW_Y = 456;
constexpr int16_t DOT_SIZE  = 10;
constexpr int16_t DOT_GAP   = 10;

struct SetupState {
    lv_obj_t*    content;         // re-built per step
    lv_obj_t*    dots_row;        // dots
    lv_obj_t*    dot_objs[4];
    lv_obj_t*    passkey_modal;
    lv_obj_t*    pairing_spinner; // Step 2 spinner — hidden while passkey modal is up
    lv_timer_t*  complete_timer;  // 5 s auto-advance on the Complete step
    screen_setup::Step step;
};

void clear_dots(SetupState* s) {
    for (int i = 0; i < 4; ++i) {
        lv_obj_set_style_bg_color(s->dot_objs[i],
            theme::color(theme::COLOR_TEXT_TERTIARY), 0);
        lv_obj_set_style_opa(s->dot_objs[i], LV_OPA_COVER, 0);
    }
}

void set_dots_active(SetupState* s, int active_index) {
    for (int i = 0; i < 4; ++i) {
        if (i == active_index) {
            lv_obj_set_style_bg_color(s->dot_objs[i],
                theme::color(theme::COLOR_ACCENT), 0);
            lv_obj_set_style_opa(s->dot_objs[i], LV_OPA_COVER, 0);
        } else if (i < active_index) {
            // done — accent at 50%
            lv_obj_set_style_bg_color(s->dot_objs[i],
                theme::color(theme::COLOR_ACCENT), 0);
            lv_obj_set_style_opa(s->dot_objs[i], LV_OPA_50, 0);
        } else {
            lv_obj_set_style_bg_color(s->dot_objs[i],
                theme::color(theme::COLOR_TEXT_TERTIARY), 0);
            lv_obj_set_style_opa(s->dot_objs[i], LV_OPA_COVER, 0);
        }
    }
}


// "Brand mark" — mirrors brandMarkHTML() in the prototype exactly.
// SVG #i-ori-logo: circle r=44 stroke=3 (outer ring) + circle r=14 (inner dot),
// both in a 100px viewBox rendered at `size` px.
// At size=132: ring outer diam ≈ 120px, dot diam ≈ 37px.
// The decorative outer ring (170px) is the .bm-ring-outer from the HTML.
lv_obj_t* make_brand_mark(lv_obj_t* parent, int16_t size) {
    // All dimensions scaled from HTML brandMarkHTML(132).
    const int16_t ring_sz   = (int16_t)((int32_t)size * 120 / 132); // 120 at size=132
    const int16_t dot_diam  = (int16_t)((int32_t)size *  37 / 132); //  37 at size=132
    const int16_t outer_sz  = (int16_t)((int32_t)size * 170 / 132); // 170 at size=132
    const int16_t inner_sz  = (int16_t)((int32_t)size * 145 / 132); // 145 at size=132
    const int16_t ring_top  = (size - ring_sz)  / 2;  //  +6 at size=132
    const int16_t outer_top = (size - outer_sz) / 2;  // -19 at size=132 (overflows root top)
    const int16_t inner_top = (size - inner_sz) / 2;  //  -6 at size=132

    lv_obj_t* root = lv_obj_create(parent);
    // Height: ring_top(6) + ring_sz(120) + 30px gap + 36px wordmark + 4px margin-bottom = 196;
    // size+64 = 196 matches HTML .brand-mark { margin-bottom: 4px }.
    lv_obj_set_size(root, size + 120, size + 64);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
    // Allow all rings and glow shadows to render outside the root bounds.
    lv_obj_add_flag(root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Atmospheric glow blob — mirrors HTML radial-gradient(circle, rgba(224,184,106,0.38) 0%, 0% 70%).
    // Transparent circle with a large circular shadow creates a ~260px soft radial glow.
    // Drawn first (lowest z-order) so all rings render on top of it.
    lv_obj_t* glow = lv_obj_create(root);
    lv_obj_set_size(glow, ring_sz, ring_sz);
    lv_obj_align(glow, LV_ALIGN_TOP_MID, 0, ring_top);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(glow, 0, 0);
    lv_obj_set_style_pad_all(glow, 0, 0);
    // spread=8 + width=60: shadow extends 68px beyond the 120px ring edge → ~256px total diameter.
    lv_obj_set_style_shadow_color(glow, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(glow, 12, 0);
    lv_obj_set_style_shadow_spread(glow, 2, 0);
    lv_obj_set_style_shadow_opa(glow, LV_OPA_60, 0);
    lv_obj_clear_flag(glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(glow, LV_OBJ_FLAG_CLICKABLE);

    // Decorative outer ring — .bm-ring-outer: 170px, 1px border, ~30% opacity.
    lv_obj_t* outer_ring = lv_obj_create(root);
    lv_obj_set_size(outer_ring, outer_sz, outer_sz);
    lv_obj_align(outer_ring, LV_ALIGN_TOP_MID, 0, outer_top);
    lv_obj_set_style_radius(outer_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(outer_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(outer_ring, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(outer_ring, 1, 0);
    lv_obj_set_style_border_opa(outer_ring, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(outer_ring, 0, 0);
    lv_obj_clear_flag(outer_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(outer_ring, LV_OBJ_FLAG_CLICKABLE);

    // Inner ring — .bm-ring-inner (HTML dashed): 145px, 1px border, ~20% opacity.
    // LVGL has no dashed border; solid thin ring at low opacity approximates the depth cue.
    lv_obj_t* inner_ring = lv_obj_create(root);
    lv_obj_set_size(inner_ring, inner_sz, inner_sz);
    lv_obj_align(inner_ring, LV_ALIGN_TOP_MID, 0, inner_top);
    lv_obj_set_style_radius(inner_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(inner_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(inner_ring, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(inner_ring, 1, 0);
    lv_obj_set_style_border_opa(inner_ring, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(inner_ring, 0, 0);
    lv_obj_clear_flag(inner_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(inner_ring, LV_OBJ_FLAG_CLICKABLE);

    // Main logo ring — SVG circle r=44 stroke=3, scaled to ring_sz.
    lv_obj_t* ring = lv_obj_create(root);
    lv_obj_set_size(ring, ring_sz, ring_sz);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, ring_top);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(ring, 4, 0);
    // Tight halo on the ring itself — approximates CSS drop-shadow(0 0 12px …) on the SVG.
    // The separate glow blob handles the large ambient aura.
    lv_obj_set_style_shadow_color(ring, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(ring, 20, 0);
    lv_obj_set_style_shadow_spread(ring, 6, 0);
    lv_obj_set_style_shadow_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    // Inner filled dot — SVG circle r=14, scaled to dot_diam.
    lv_obj_t* dot = lv_obj_create(ring);
    lv_obj_set_size(dot, dot_diam, dot_diam);
    lv_obj_center(dot);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    // Subtle inner-core glow so the center dot reads as luminous, not flat.
    lv_obj_set_style_shadow_color(dot, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(dot, 24, 0);
    lv_obj_set_style_shadow_spread(dot, 8, 0);
    lv_obj_set_style_shadow_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    // Wordmark row: [line_left] [gap] ["ori"] [gap] [line_right]
    // 30px below the ring bottom (ring_top + ring_sz + 30).
    lv_obj_t* word_row = lv_obj_create(root);
    lv_obj_set_size(word_row, size + 120, 36);
    lv_obj_align(word_row, LV_ALIGN_TOP_MID, 0, ring_top + ring_sz + 30);
    lv_obj_set_style_bg_opa(word_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(word_row, 0, 0);
    lv_obj_set_style_pad_all(word_row, 0, 0);
    lv_obj_set_style_pad_column(word_row, 10, 0);
    lv_obj_clear_flag(word_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(word_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(word_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(word_row,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Blended accent at ~58% over COLOR_BG (#0A0D11):
    //   R = 0.58*224 + 0.42*10 = 134  → 0x86
    //   G = 0.58*184 + 0.42*13 = 112  → 0x70
    //   B = 0.58*106 + 0.42*17 =  69  → 0x45
    constexpr uint32_t ACCENT_58 = 0x867045;

    // Left flanking line: gradient from BG → blended accent (fade in from left).
    lv_obj_t* line_l = lv_obj_create(word_row);
    lv_obj_set_size(line_l, 44, 2);
    lv_obj_set_style_radius(line_l, 1, 0);
    lv_obj_set_style_bg_color(line_l, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_grad_color(line_l, theme::color(ACCENT_58), 0);
    lv_obj_set_style_bg_grad_dir(line_l, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(line_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line_l, 0, 0);
    lv_obj_set_style_pad_all(line_l, 0, 0);
    lv_obj_clear_flag(line_l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line_l, LV_OBJ_FLAG_CLICKABLE);

    // "ori" wordmark — 'r' coloured accent. No extra spaces; letter_space=10 handles gaps.
    lv_obj_t* word = lv_label_create(word_row);
    lv_label_set_text(word, "o#E0B86A r#i");
    lv_label_set_recolor(word, true);
    lv_obj_set_style_text_color(word, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(word, theme::font_time(), 0);
    lv_obj_set_style_text_letter_space(word, 10, 0);

    // Right flanking line: gradient from blended accent → BG (fade out to right).
    lv_obj_t* line_r = lv_obj_create(word_row);
    lv_obj_set_size(line_r, 44, 2);
    lv_obj_set_style_radius(line_r, 1, 0);
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

// "Pill" — accent-soft background, accent text, rounded. Used for BLE name.
lv_obj_t* make_ble_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* pill = lv_obj_create(parent);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, theme::color(theme::COLOR_ACCENT_SOFT), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 16, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_left(pill, 32, 0);
    lv_obj_set_style_pad_right(pill, 32, 0);
    lv_obj_set_style_pad_top(pill, 16, 0);
    lv_obj_set_style_pad_bottom(pill, 16, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(pill);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(lbl, theme::font_large(), 0);
    lv_obj_center(lbl);
    return pill;
}

// 30 fps spinner — lv_timer at 33 ms replaces lv_spinner_create's 60 fps lv_anim.
// 8° step × 45 ticks × 33 ms ≈ 1485 ms/rev (≈ original 1400 ms).
constexpr uint32_t SPIN_INTERVAL_MS = 42;   // 24 fps
constexpr uint16_t SPIN_STEP_DEG    = 10;  // 10° × 36 steps × 42 ms ≈ 1512 ms/rev

struct SpinnerState {
    uint16_t    rotation;
    lv_timer_t* timer;
};

static void spinner_timer_cb(lv_timer_t* t) {
    lv_obj_t* arc = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
    auto* s = static_cast<SpinnerState*>(lv_obj_get_user_data(arc));
    s->rotation = (uint16_t)((s->rotation + SPIN_STEP_DEG) % 360);
    lv_arc_set_rotation(arc, s->rotation);
}

// Big spinning ring — Step 2 & Step 4 pairing animation.
lv_obj_t* make_spinner(lv_obj_t* parent, int16_t size) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, theme::color(theme::COLOR_DIVIDER_STRONG), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, theme::color(theme::COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, 0, 60);

    auto* s = new SpinnerState{0, nullptr};
    lv_obj_set_user_data(arc, s);
    s->timer = lv_timer_create(spinner_timer_cb, SPIN_INTERVAL_MS, arc);
    lv_obj_add_event_cb(arc, [](lv_event_t* e) {
        auto* ss = static_cast<SpinnerState*>(
            lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e)));
        if (ss) { lv_timer_delete(ss->timer); delete ss; }
    }, LV_EVENT_DELETE, nullptr);
    return arc;
}

// Animation callback for the check-ring arc.
static void ring_anim_cb(void* obj, int32_t val) {
    lv_arc_set_value(static_cast<lv_obj_t*>(obj), (int16_t)val);
}

// One-shot timer: reveal the tick mark once the ring sweep is nearly done.
static void reveal_tick_cb(lv_timer_t* t) {
    lv_obj_clear_flag(static_cast<lv_obj_t*>(lv_timer_get_user_data(t)), LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(t);
}

// Animated "OK" check — a 130×130 arc ring that sweeps clockwise in 550 ms,
// followed by a tick mark that appears at 450 ms. Replaces the old static
// filled circle. Approximates the SVG stroke-dashoffset animation in the
// HTML prototype.
lv_obj_t* make_ok_check(lv_obj_t* parent) {
    // Transparent container — holds arc + tick; provides the green glow shadow.
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, 130, 130);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, LV_RADIUS_CIRCLE, 0);
    // Green ambient glow — approximates CSS filter:drop-shadow on the SVG.
    lv_obj_set_style_shadow_color(root, theme::color(theme::COLOR_OK), 0);
    lv_obj_set_style_shadow_width(root, 22, 0);
    lv_obj_set_style_shadow_opa(root, LV_OPA_40, 0);
    lv_obj_set_style_shadow_spread(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);

    // Arc: background track (dim green ring) + animated indicator.
    lv_obj_t* arc = lv_arc_create(root);
    lv_obj_set_size(arc, 130, 130);
    lv_obj_center(arc);
    // Full-circle background track — dim green.
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_value(arc, 0);              // indicator starts empty
    lv_arc_set_rotation(arc, 270);         // start sweep at 12 o'clock
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc, theme::color(theme::COLOR_OK), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, theme::color(theme::COLOR_OK), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

    // Checkmark via two-segment lv_line — hidden until reveal timer fires.
    // Points are inside the 130×130 frame, matching prototype SVG path
    // "M29 51 L43 65 L71 35" scaled to 130 px.
    static lv_point_precise_t tick_pts[3] = { {27, 65}, {52, 87}, {95, 43} };
    lv_obj_t* tick = lv_line_create(root);
    lv_line_set_points(tick, tick_pts, 3);
    lv_obj_set_style_line_color(tick, theme::color(theme::COLOR_OK), 0);
    lv_obj_set_style_line_width(tick, 6, 0);
    lv_obj_set_style_line_rounded(tick, true, 0);
    lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);

    // Ring sweep animation: arc value 0→100 in 1000 ms, ease-out.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, ring_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    // Reveal tick after 800 ms (ring is ~82% done — feels simultaneous to eye).
    lv_timer_t* t = lv_timer_create(reveal_tick_cb, 800, tick);
    lv_timer_set_repeat_count(t, 1);

    return root;
}

// Forward declarations of step event handlers.
void on_start_clicked(lv_event_t* e);
void on_next_clicked(lv_event_t* e);
void on_skip_clicked(lv_event_t* e);
void on_complete_clicked(lv_event_t* e);

// Each step builder populates `content`. Caller has already cleared it.
void build_welcome(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    // 8 px top spacer — HTML: brandMarkHTML has style="margin-top:8px".
    lv_obj_t* pre = lv_obj_create(content);
    lv_obj_set_size(pre, 1, 8);
    lv_obj_set_style_bg_opa(pre, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pre, 0, 0);
    lv_obj_set_style_pad_all(pre, 0, 0);
    make_brand_mark(content, 132);

    lv_obj_t* h = lv_label_create(content);
    lv_label_set_text(h, "Welcome aboard");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(h, 4, 0);

    lv_obj_t* p = lv_label_create(content);
    lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
    lv_label_set_text(p,
        "A calm display for meetings, presence, and quiet awareness");
    lv_obj_set_width(p, lv_pct(100));
    lv_obj_set_style_text_color(p, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p, theme::font_meta(), 0);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p, 8, 0);
    lv_obj_set_style_pad_bottom(p, 24, 0);

    // Primary ghost button: "START" uppercase, font_body (30px), accent glow.
    ui::make_btn(content, "START",
        ui::BtnStyle::Primary,
        on_start_clicked, screen,
        30, 66, theme::font_time());
}

void build_install(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    // Ring size matches Welcome (132) for a seamless page transition.
    // 8 px top spacer — mirrors HTML brandMarkHTML's style="margin-top:8px".
    lv_obj_t* pre = lv_obj_create(content);
    lv_obj_set_size(pre, 1, 8);
    lv_obj_set_style_bg_opa(pre, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pre, 0, 0);
    lv_obj_set_style_pad_all(pre, 0, 0);
    make_brand_mark(content, 132);

    lv_obj_t* h = lv_label_create(content);
    lv_label_set_text(h, "Install the Orion app on your PC");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(h, 4, 0);

    lv_obj_t* p = lv_label_create(content);
    lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(p, true);
    lv_label_set_text(p,
        "Visit #E0B86A ori.app/orion# on your PC. "
        "Orion runs on Windows and macOS.");
    lv_obj_set_width(p, lv_pct(100));
    lv_obj_set_style_text_color(p, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p, theme::font_meta(), 0);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p, 8, 0);
    lv_obj_set_style_pad_bottom(p, 24, 0);

    // Primary ghost button: "NEXT" uppercase, font_body (30px), accent glow.
    ui::make_btn(content, "NEXT",
        ui::BtnStyle::Primary,
        on_next_clicked, screen,
        30, 76, theme::font_time());
}

void build_pairing(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    (void)screen;

    lv_obj_t* h = lv_label_create(content);
    lv_label_set_text(h, "Orion pairing");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* p = lv_label_create(content);
    lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
    lv_label_set_text(p, "Open Orion on your computer and select this device.");
    lv_obj_set_width(p, 800);
    lv_obj_set_style_text_color(p, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p, theme::font_meta(), 0);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p, 16, 0);
    lv_obj_set_style_pad_bottom(p, 16, 0);

    lv_obj_t* pill = make_ble_pill(content, mock_data::ble_name());
    lv_obj_set_style_pad_top(pill, 16, 0);

    lv_obj_t* spinner = make_spinner(content, 100);
    lv_obj_set_style_pad_top(spinner, 24, 0);
    s->pairing_spinner = spinner;

    lv_obj_t* hint = lv_label_create(content);
    lv_label_set_text(hint, "Ori will continue automatically once PC is connected");
    lv_obj_set_style_text_color(hint, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(hint, theme::font_meta(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(hint, 14, 0);
}

void build_orioning(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    (void)s;
    (void)screen;

    lv_obj_t* h = lv_label_create(content);
    lv_label_set_text(h, "Getting things ready");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(h, 56, 0);

    lv_obj_t* ring = widget_progress_ring::create(content, 200);
    lv_obj_set_style_pad_top(ring, 12, 0);
    lv_obj_set_style_pad_bottom(ring, 12, 0);
    widget_progress_ring::set_value(ring, 67);
    widget_progress_ring::set_label_text(ring, "67%");

    lv_obj_t* sub = lv_label_create(content);
    lv_label_set_text(sub, "Looks like a busy day ahead");
    lv_obj_set_style_text_color(sub, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(sub, theme::font_meta(), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(sub, 14, 0);
}

void build_phone_pairing(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    (void)s;

    lv_obj_t* h = lv_label_create(content);
    lv_label_set_text(h, "Phone pairing");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* p = lv_label_create(content);
    lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
    lv_label_set_text(p,
        "Ori provides quiet notification awareness via Bluetooth connection");
    lv_obj_set_width(p, 800);
    lv_obj_set_style_text_color(p, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p, theme::font_meta(), 0);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p, 16, 0);
    lv_obj_set_style_pad_bottom(p, 16, 0);

    lv_obj_t* pill = make_ble_pill(content, mock_data::ble_name());
    lv_obj_set_style_pad_top(pill, 22, 0);

    lv_obj_t* spinner = make_spinner(content, 100);
    lv_obj_set_style_pad_top(spinner, 24, 0);

    lv_obj_t* hint = lv_label_create(content);
    lv_label_set_text(hint, "Ori will continue automatically once phone is connected");
    lv_obj_set_style_text_color(hint, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(hint, theme::font_meta(), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(hint, 14, 0);
    lv_obj_set_style_pad_bottom(hint, 18, 0);

    // Tertiary ghost button — low visual weight; phone pairing is optional.
    ui::make_btn(content, "Skip for now",
        ui::BtnStyle::Tertiary,
        on_skip_clicked, screen,
        14, 56, theme::font_time());
}

// Timer callback: 5 s elapsed without user interaction → advance to runtime.
static void complete_timer_cb(lv_timer_t* t) {
    SetupState* s = static_cast<SetupState*>(lv_timer_get_user_data(t));
    if (s) s->complete_timer = nullptr;
    Serial.println("[setup] Complete timer fired — transitioning to runtime");
    state_machine::on_setup_complete();
}

void build_complete(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    (void)screen;

    make_ok_check(content);

    lv_obj_t* h = lv_label_create(content);
    lv_label_set_text(h, "Ori is ready");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(h, 18, 0);

    lv_obj_t* p = lv_label_create(content);
    // "Xander" is a mock placeholder — at runtime this will use the first
    // name from the profile synced by Orion on initial setup completion.
    lv_label_set_text(p, "Welcome, Xander.");
    lv_obj_set_style_text_color(p, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p, theme::font_meta(), 0);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p, 12, 0);
    lv_obj_set_style_pad_bottom(p, 48, 0);

    // Primary ghost button — user can tap to advance immediately.
    // User data is `s` so the callback can cancel the auto-advance timer.
    ui::make_btn(content, "LET'S GET TO WORK",
        ui::BtnStyle::Primary,
        on_complete_clicked, s,
        36, 54, theme::font_h2());

    // 5 s auto-advance: if the user does nothing, transition to runtime.
    // Stored in s->complete_timer so rebuild_for() can cancel it if the
    // step ever changes before the timer fires (e.g. during testing).
    if (s->complete_timer) {
        lv_timer_delete(s->complete_timer);
    }
    s->complete_timer = lv_timer_create(complete_timer_cb, 5000, s);
    lv_timer_set_repeat_count(s->complete_timer, 1);
}

void rebuild_for(lv_obj_t* screen, SetupState* s) {
    // Cancel any pending auto-advance timer from a previous Complete render.
    if (s->complete_timer) {
        lv_timer_delete(s->complete_timer);
        s->complete_timer = nullptr;
    }

    if (s->content) {
        lv_obj_delete(s->content);
        s->content = nullptr;
    }
    s->pairing_spinner = nullptr;
    s->content = lv_obj_create(screen);
    lv_obj_set_size(s->content, 800, 480);
    lv_obj_set_pos(s->content, 0, 0);
    lv_obj_set_style_bg_color(s->content, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s->content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s->content, 0, 0);
    // pad_top=20 = content-padding(20)
    lv_obj_set_style_pad_top(s->content, 24, 0);
    lv_obj_set_style_pad_left(s->content, 0, 0);
    lv_obj_set_style_pad_right(s->content, 0, 0);
    lv_obj_set_style_pad_row(s->content, 0, 0);
    lv_obj_set_style_pad_column(s->content, 0, 0);
    lv_obj_clear_flag(s->content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->content, LV_OBJ_FLAG_CLICKABLE);
    // Allow the brand-mark glow blob to bleed above the content top edge.
    lv_obj_add_flag(s->content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(s->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s->content,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    using Step = screen_setup::Step;
    switch (s->step) {
        case Step::Welcome:      build_welcome(s->content, s, screen);       clear_dots(s);            break;
        case Step::Install:      build_install(s->content, s, screen);       set_dots_active(s, 0);    break;
        case Step::Pairing:      build_pairing(s->content, s, screen);       set_dots_active(s, 1);    break;
        case Step::Orioning:     build_orioning(s->content, s, screen);      set_dots_active(s, 2);    break;
        case Step::PhonePairing: build_phone_pairing(s->content, s, screen); set_dots_active(s, 3);    break;
        case Step::Complete:     build_complete(s->content, s, screen);                                break;
    }

    // Complete hides the dots; every other step shows them.
    if (s->step == Step::Complete) {
        lv_obj_add_flag(s->dots_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s->dots_row, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(s->dots_row);
}

void on_start_clicked(lv_event_t* e) {
    lv_obj_t* screen = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    screen_setup::set_step(screen, screen_setup::Step::Install);
}

void on_next_clicked(lv_event_t* e) {
    lv_obj_t* screen = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    screen_setup::set_step(screen, screen_setup::Step::Pairing);
}

void on_skip_clicked(lv_event_t* e) {
    lv_obj_t* screen = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    screen_setup::set_step(screen, screen_setup::Step::Complete);
}

void on_complete_clicked(lv_event_t* e) {
    // User tapped "LET'S GET TO WORK" — cancel the auto-advance timer and
    // transition immediately to the runtime screen.
    SetupState* s = static_cast<SetupState*>(lv_event_get_user_data(e));
    if (s && s->complete_timer) {
        lv_timer_delete(s->complete_timer);
        s->complete_timer = nullptr;
    }
    Serial.println("[setup] Complete tapped — transitioning to runtime");
    state_machine::on_setup_complete();
}

} // namespace

namespace screen_setup {

lv_obj_t* create(Step initial) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    auto* s = new SetupState();
    s->content         = nullptr;
    s->passkey_modal   = nullptr;
    s->pairing_spinner = nullptr;
    s->complete_timer  = nullptr;
    s->step           = initial;

    // Dot row — anchored at the fixed y so it never moves between pages.
    s->dots_row = lv_obj_create(screen);
    int16_t dot_row_width = 4 * DOT_SIZE + 3 * DOT_GAP;
    lv_obj_set_size(s->dots_row, dot_row_width, DOT_SIZE);
    lv_obj_set_pos(s->dots_row, (800 - dot_row_width) / 2, DOT_ROW_Y);
    lv_obj_set_style_bg_opa(s->dots_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s->dots_row, 0, 0);
    lv_obj_set_style_pad_all(s->dots_row, 0, 0);
    lv_obj_set_style_pad_column(s->dots_row, DOT_GAP, 0);
    lv_obj_clear_flag(s->dots_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s->dots_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(s->dots_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s->dots_row,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 4; ++i) {
        lv_obj_t* dot = lv_obj_create(s->dots_row);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        s->dot_objs[i] = dot;
    }

    lv_obj_set_user_data(screen, s);
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        auto* ss = static_cast<SetupState*>(lv_event_get_user_data(e));
        if (ss && ss->complete_timer) {
            lv_timer_delete(ss->complete_timer);
            ss->complete_timer = nullptr;
        }
        delete ss;
    }, LV_EVENT_DELETE, s);
    rebuild_for(screen, s);
    return screen;
}

void set_step(lv_obj_t* screen, Step st) {
    auto* s = static_cast<SetupState*>(lv_obj_get_user_data(screen));
    if (!s) return;
    s->step = st;
    rebuild_for(screen, s);
}

lv_obj_t* show_passkey_modal(lv_obj_t* screen) {
    auto* s = static_cast<SetupState*>(lv_obj_get_user_data(screen));
    if (!s) return nullptr;
    if (s->passkey_modal) return s->passkey_modal;

    // Spinner is invisible under the scrim — stop its animation to save CPU.
    if (s->pairing_spinner) lv_obj_add_flag(s->pairing_spinner, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* scrim = lv_obj_create(screen);
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, theme::color(theme::COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, theme::SCRIM_OPA, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* card = lv_obj_create(scrim);
    lv_obj_set_size(card, 540, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, theme::color(theme::COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, theme::color(theme::COLOR_DIVIDER_STRONG), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 36, 0);
    lv_obj_set_style_shadow_color(card, theme::color(0x000000), 0);
    lv_obj_set_style_shadow_width(card, 30, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_70, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* h = lv_label_create(card);
    lv_label_set_text(h, "Confirm this passkey on Orion");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_h2(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* digits = lv_label_create(card);
    lv_label_set_text(digits, mock_data::passkey());
    lv_obj_set_style_text_color(digits, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_font(digits, theme::font_large(), 0);
    lv_obj_set_style_text_letter_space(digits, 12, 0);
    lv_obj_set_style_text_align(digits, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(digits, 22, 0);

    s->passkey_modal = scrim;
    return scrim;
}

void hide_passkey_modal(lv_obj_t* screen) {
    auto* s = static_cast<SetupState*>(lv_obj_get_user_data(screen));
    if (!s || !s->passkey_modal) return;
    lv_obj_delete(s->passkey_modal);
    s->passkey_modal = nullptr;
    if (s->pairing_spinner) lv_obj_clear_flag(s->pairing_spinner, LV_OBJ_FLAG_HIDDEN);
}

} // namespace screen_setup
