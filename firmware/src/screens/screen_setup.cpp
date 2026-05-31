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
    lv_obj_t*    orioning_modal;  // Orioning sync modal (child of screen, not content)
    lv_obj_t*    pairing_spinner; // Step 2 spinner — hidden while passkey modal is up
    lv_obj_t*    countdown_bar;   // Setup complete countdown bar
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


// Wordmark-only brand mark — flanking gradient lines + "ori" label row.
// Mirrors brandMarkHTML() after the logo ring was removed from the prototype.
// margin-top: -2 positions it 10 px higher than the old 8 px spacer approach.
lv_obj_t* make_brand_mark(lv_obj_t* parent) {
    // Accent at 58% over pure black: R=0.58*224=130→0x82, G=0.58*184=107→0x6B, B=0.58*106=61→0x3D
    constexpr uint32_t ACCENT_58 = 0x826B3D;

    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_column(root, 10, 0);
    lv_obj_set_style_margin_top(root, -2, 0);
    lv_obj_set_style_margin_bottom(root, 4, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* line_l = lv_obj_create(root);
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

    lv_obj_t* word = lv_label_create(root);
    lv_label_set_text(word, "o#E0B86A r#i");
    lv_label_set_recolor(word, true);
    lv_obj_set_style_text_color(word, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(word, theme::font_time(), 0);
    lv_obj_set_style_text_letter_space(word, 10, 0);

    lv_obj_t* line_r = lv_obj_create(root);
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

// Flex-grow centred container — fills remaining vertical space between the brand
// mark and any bottom button, then centres its children vertically within that space.
lv_obj_t* make_mid(lv_obj_t* parent) {
    lv_obj_t* mid = lv_obj_create(parent);
    lv_obj_set_width(mid, lv_pct(100));
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return mid;
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
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
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
void on_skip_phone_clicked(lv_event_t* e);

// Each step builder populates `content`. Caller has already cleared it.
// For screens with a bottom button, pad_bottom=54 anchors the button 30 px above
// the dot row (DOT_ROW_Y=456; button bottom = 456-30=426; pad_bottom=480-426=54).
void build_welcome(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    lv_obj_set_style_pad_bottom(content, 54, 0);
    make_brand_mark(content);

    lv_obj_t* mid = make_mid(content);

    lv_obj_t* p1 = lv_label_create(mid);
    lv_label_set_text(p1, "A display for your day");
    lv_obj_set_style_text_color(p1, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(p1, theme::font_display(), 0);
    lv_obj_set_style_text_align(p1, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* p2 = lv_label_create(mid);
    lv_label_set_text(p2, "Your desk deserves better");
    lv_obj_set_style_text_color(p2, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p2, theme::font_title(), 0);
    lv_obj_set_style_text_align(p2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p2, 10, 0);

    ui::make_btn(content, "START",
        ui::BtnStyle::Primary,
        on_start_clicked, screen,
        30, 60, theme::font_time());
}

void build_install(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    lv_obj_set_style_pad_bottom(content, 54, 0);
    make_brand_mark(content);

    lv_obj_t* mid = make_mid(content);

    lv_obj_t* p1 = lv_label_create(mid);
    lv_label_set_long_mode(p1, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(p1, true);
    lv_label_set_text(p1, "Download Orion at #E0B86A ori.app/orion#");
    lv_obj_set_width(p1, lv_pct(100));
    lv_obj_set_style_text_color(p1, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(p1, theme::font_display(), 0);
    lv_obj_set_style_text_align(p1, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* p2 = lv_label_create(mid);
    lv_label_set_text(p2, "Available on Windows and macOS");
    lv_obj_set_width(p2, lv_pct(100));
    lv_obj_set_style_text_color(p2, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p2, theme::font_title(), 0);
    lv_obj_set_style_text_align(p2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p2, 10, 0);

    ui::make_btn(content, "NEXT",
        ui::BtnStyle::Primary,
        on_next_clicked, screen,
        30, 60, theme::font_time());
}

void build_pairing(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    (void)screen;
    make_brand_mark(content);

    lv_obj_t* mid = make_mid(content);

    lv_obj_t* h = lv_label_create(mid);
    lv_label_set_text(h, "Connect to this device on Orion");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* pill = make_ble_pill(mid, mock_data::ble_name());
    lv_obj_set_style_pad_top(pill, 20, 0);

    lv_obj_t* spinner = make_spinner(mid, 100);
    lv_obj_set_style_pad_top(spinner, 24, 0);
    s->pairing_spinner = spinner;
}

void build_orioning(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    // Build Step 2 Pairing as the background.
    make_brand_mark(content);
    lv_obj_t* mid = make_mid(content);

    lv_obj_t* h = lv_label_create(mid);
    lv_label_set_text(h, "Connect to this device on Orion");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* pill = make_ble_pill(mid, mock_data::ble_name());
    lv_obj_set_style_pad_top(pill, 10, 0);

    // Orioning modal card — same visual container as the passkey modal.
    lv_obj_t* scrim = lv_obj_create(screen);
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, theme::color(theme::COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, theme::SCRIM_OPA, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_CLICKABLE);

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

    lv_obj_t* heading = lv_label_create(card);
    lv_label_set_text(heading, "A busy day ahead\xe2\x80\xa6");
    lv_obj_set_style_text_color(heading, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(heading, theme::font_h2(), 0);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* ring = widget_progress_ring::create(card, 140);
    lv_obj_set_style_pad_top(ring, 24, 0);
    widget_progress_ring::set_value(ring, 67);
    widget_progress_ring::set_label_font(ring, theme::font_time());
    widget_progress_ring::set_label_text_center(ring, "67%");

    s->orioning_modal = scrim;
    lv_obj_move_foreground(scrim);
}

void build_phone_pairing(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    (void)s;
    lv_obj_set_style_pad_bottom(content, 54, 0);
    make_brand_mark(content);

    lv_obj_t* mid = make_mid(content);

    lv_obj_t* h = lv_label_create(mid);
    lv_label_set_text(h, "Connect to this device on iPhone");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* pill = make_ble_pill(mid, mock_data::ble_name());
    lv_obj_set_style_pad_top(pill, 10, 0);

    lv_obj_t* spinner = make_spinner(mid, 100);
    lv_obj_set_style_pad_top(spinner, 24, 0);

    ui::make_btn(content, "Skip",
        ui::BtnStyle::Tertiary,
        on_skip_phone_clicked, screen,
        14, 40, theme::font_time());
}

// Timer callback: 5 s elapsed without user interaction → advance to runtime.
static void complete_timer_cb(lv_timer_t* t) {
    SetupState* s = static_cast<SetupState*>(lv_timer_get_user_data(t));
    if (s) s->complete_timer = nullptr;
    Serial.println("[setup] Complete timer fired — transitioning to runtime");
    state_machine::on_setup_complete();
}

void build_complete(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    make_brand_mark(content);

    // Flex-grow block: checkmark + text centred in the remaining height.
    lv_obj_t* mid = make_mid(content);

    make_ok_check(mid);

    // "Everstorm" is the mock first name — at runtime use the first token of the
    // profile name synced by Orion.
    lv_obj_t* h = lv_label_create(mid);
    lv_label_set_text(h, "Welcome, Everstorm");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(h, 20, 0);

    lv_obj_t* sub = lv_label_create(mid);
    lv_label_set_text(sub, "Let's get to work");
    lv_obj_set_style_text_color(sub, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(sub, theme::font_title(), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(sub, 10, 0);

    // Countdown bar — 3 px accent strip at the very bottom of the screen.
    // Drains from full width to zero in 5000 ms, synced with the auto-advance timer.

    // Create and track the countdown bar
    lv_obj_t* bar = lv_bar_create(screen);
    s->countdown_bar = bar;
    lv_obj_set_size(bar, 800, 3);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, theme::color(theme::COLOR_ELEV), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, theme::color(theme::COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t val) {
        lv_bar_set_value(static_cast<lv_obj_t*>(obj), (int16_t)val, LV_ANIM_OFF);
    });
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 5000);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);

    if (s->complete_timer) lv_timer_delete(s->complete_timer);
    s->complete_timer = lv_timer_create(complete_timer_cb, 5000, s);
    lv_timer_set_repeat_count(s->complete_timer, 1);
}

void rebuild_for(lv_obj_t* screen, SetupState* s) {
    // Cancel any pending auto-advance timer from a previous Complete render.
    if (s->complete_timer) {
        lv_timer_delete(s->complete_timer);
        s->complete_timer = nullptr;
    }

    // Cancel countdown bar animation if it exists
    if (s->countdown_bar) {
        lv_anim_del(s->countdown_bar, NULL);
        s->countdown_bar = nullptr;
    }

    if (s->orioning_modal) {
        lv_obj_delete(s->orioning_modal);
        s->orioning_modal = nullptr;
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
        case Step::Orioning:     build_orioning(s->content, s, screen);      set_dots_active(s, 1);    break;
        case Step::PhonePairing: build_phone_pairing(s->content, s, screen); set_dots_active(s, 2);    break;
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

void on_skip_phone_clicked(lv_event_t* e) {
    lv_obj_t* screen = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    screen_setup::set_step(screen, screen_setup::Step::Complete);
}

} // namespace

namespace screen_setup {

lv_obj_t* create(Step initial) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    auto* s = new SetupState();
    s->content         = nullptr;
    s->passkey_modal   = nullptr;
    s->orioning_modal  = nullptr;
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
