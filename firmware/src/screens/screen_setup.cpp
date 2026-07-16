#include "screens/screen_setup.h"

#include <Arduino.h>
#include "ori_log.h"
#include <lvgl.h>

#include "app_state.h"
#include "ble/ble_manager.h"
#include "nvs_store.h"
#include "state_machine.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_progress_ring.h"

// Setup wizard — single LVGL screen, five sub-states. Status bar is OFF
// across the entire flow.
//
// Layout invariant (setup-flow.md):
//   - Step dots are anchored at y = 456 (i.e. 24 px above the bottom of the
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
    lv_obj_t*    dot_objs[3];
    lv_obj_t*    passkey_modal;
    lv_obj_t*    orioning_modal;  // Orioning sync modal (child of screen, not content)
    lv_obj_t*    orioning_ring;   // progress ring inside orioning_modal
    lv_obj_t*    pairing_spinner; // Step 2 spinner — hidden while passkey modal is up
    lv_obj_t*    countdown_bar;   // Setup complete countdown bar
    lv_timer_t*  complete_timer;  // 5 s auto-advance on the Complete step
    lv_obj_t*    prev_screen;     // non-null during runtime re-pair: Skip goes back here
    screen_setup::Step step;
    uint32_t     id;              // creation order — see g_next_setup_id
};

// Bumped by every screen_setup::create() call. Lets a screen's own DELETE
// handler tell whether it was superseded by a NEWER screen_setup screen (id
// still current -> genuinely leaving the wizard/step; id stale -> just torn
// down and replaced in place, e.g. by a spurious top-level rebuild).
uint32_t g_next_setup_id = 0;

// Mirrors the step of the currently-displayed setup screen (updated wherever
// rebuild_for() runs — the single funnel for both create() and set_step()).
// Lets ble_manager::is_orion_pairing_allowed() gate the Orion bond slot to
// Setup Step 2 specifically (ble-protocol.md §2), instead of the whole
// first-boot flow — AppState::SETUP alone can't distinguish Welcome/Install/
// Pairing/PhonePairing/Complete since they're all one flat AppState.
screen_setup::Step g_current_step = screen_setup::Step::Welcome;

void clear_dots(SetupState* s) {
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_color(s->dot_objs[i],
            theme::color(theme::COLOR_TEXT_TERTIARY), 0);
        lv_obj_set_style_opa(s->dot_objs[i], LV_OPA_COVER, 0);
    }
}

void set_dots_active(SetupState* s, int active_index) {
    for (int i = 0; i < 3; ++i) {
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


// Brand mark (flanking gradient lines + "ori" label) now lives in
// ui_helpers.cpp as ui::make_brand_mark() — shared with screen_boot_splash.cpp.

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

// 24 fps spinner — lv_timer at 42 ms replaces lv_spinner_create's 60 fps lv_anim.
constexpr uint32_t SPIN_INTERVAL_MS = 42;   // 24 fps
constexpr uint16_t SPIN_STEP_DEG    = 10;  // 10° × 36 steps × 42 ms ≈ 1512 ms/rev

struct SpinnerState {
    uint16_t    rotation;
    lv_timer_t* timer;
};

lv_obj_t* make_spinner(lv_obj_t* parent, int16_t size, int16_t x_offset = 0);

static void spinner_timer_cb(lv_timer_t* t) {
    lv_obj_t* arc = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
    auto* s = static_cast<SpinnerState*>(lv_obj_get_user_data(arc));
    s->rotation = (uint16_t)((s->rotation + SPIN_STEP_DEG) % 360);
    lv_arc_set_rotation(arc, s->rotation);
}

// Big spinning ring — Step 2 & Step 4 pairing animation.
lv_obj_t* make_spinner(lv_obj_t* parent, int16_t size, int16_t x_offset) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_translate_x(arc, x_offset, 0);
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
// The 800 ms delay can outlive the screen that owns `tick` (e.g. the Setup
// Complete screen's 5 s auto-advance fires in the same lv_timer_handler()
// pass after a long gap between passes, deleting `tick` before this timer
// runs). Guard against that with a matching LV_EVENT_DELETE handler on
// `tick` (see make_ok_check) — clear its user_data here so that handler
// becomes a no-op once this timer has already fired.
static void reveal_tick_cb(lv_timer_t* t) {
    lv_obj_t* tick = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(tick, nullptr);
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

    // If `tick` is deleted before the timer fires (screen torn down early —
    // see reveal_tick_cb), delete the pending timer too so it never runs
    // against a freed object.
    lv_obj_set_user_data(tick, t);
    lv_obj_add_event_cb(tick, [](lv_event_t* e) {
        auto* timer = static_cast<lv_timer_t*>(
            lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e))));
        if (timer) lv_timer_delete(timer);
    }, LV_EVENT_DELETE, nullptr);

    return root;
}

// Shared scrim+card shell for the passkey and Orioning-progress modals —
// both are a 540 px wide, content-height card centred in a full-screen scrim
// with the same card styling (bg/border/radius/padding/shadow) and a
// vertical-flex, centre-aligned content flow. Only the scrim's tap-absorb
// behaviour and the card's contents (heading + passkey digits vs. progress
// ring) differ between the two callers.
lv_obj_t* make_step_modal_card(lv_obj_t* screen, bool absorb_taps, lv_obj_t** out_scrim) {
    lv_obj_t* scrim = ui::make_scrim(screen, absorb_taps);

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

    *out_scrim = scrim;
    return card;
}

void on_start_clicked(lv_event_t* e);
void on_next_clicked(lv_event_t* e);
void on_skip_phone_clicked(lv_event_t* e);

// Each step builder populates `content`. Caller has already cleared it.
// For screens with a bottom button, pad_bottom=54 anchors the button 30 px above
// the dot row (DOT_ROW_Y=456; button bottom = 456-30=426; pad_bottom=480-426=54).
void build_welcome(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    lv_obj_set_style_pad_bottom(content, 54, 0);
    ui::make_brand_mark(content);

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

    ui::make_btn(content, "Start",
        ui::BtnStyle::Primary,
        on_start_clicked, screen,
        18, 36, theme::font_title());
}

void build_install(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    lv_obj_set_style_pad_bottom(content, 54, 0);
    ui::make_brand_mark(content);

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
    lv_label_set_text(p2, "Available on Windows and MacOS");
    lv_obj_set_width(p2, lv_pct(100));
    lv_obj_set_style_text_color(p2, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p2, theme::font_title(), 0);
    lv_obj_set_style_text_align(p2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p2, 10, 0);

    ui::make_btn(content, "Next",
        ui::BtnStyle::Primary,
        on_next_clicked, screen,
        18, 36, theme::font_title());
}

void build_pairing(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    (void)screen;
    ui::make_brand_mark(content);

    lv_obj_t* mid = make_mid(content);

    lv_obj_t* h = lv_label_create(mid);
    lv_label_set_text(h, "Connect on Orion");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* pill = make_ble_pill(mid, app_state::ble_name());
    lv_obj_set_style_pad_top(pill, 20, 0);

    lv_obj_t* spinner = make_spinner(mid, 100, 8);
    lv_obj_set_style_pad_top(spinner, 24, 0);
    s->pairing_spinner = spinner;
}

void build_phone_pairing(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    lv_obj_set_style_pad_bottom(content, 54, 0);
    ui::make_brand_mark(content);

    lv_obj_t* mid = make_mid(content);

    lv_obj_t* h = lv_label_create(mid);
    lv_label_set_text(h, "Connect on iPhone or iPad");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* pill = make_ble_pill(mid, app_state::ble_name());
    lv_obj_set_style_pad_top(pill, 10, 0);

    lv_obj_t* spinner = make_spinner(mid, 100, 8);
    // Shifted up 8px: gap above trimmed 24->16, and the same 8px re-added below
    // so the centered (title+pill+spinner) block's total height — and thus the
    // Skip button / dot row below it, both laid out outside this block — don't move.
    lv_obj_set_style_pad_top(spinner, 16, 0);
    lv_obj_set_style_pad_bottom(spinner, 8, 0);

    // Runtime re-pair (prev_screen set) just returns to the launching screen, so
    // label it "Close". During first-time setup it skips the optional iPhone/
    // iPad step and advances, so it stays "Skip". (on_skip_phone_clicked
    // branches on prev_screen to do the right thing in each case.)
    ui::make_btn(content, s->prev_screen ? "Close" : "Skip",
        ui::BtnStyle::Tertiary,
        on_skip_phone_clicked, screen,
        14, 40, theme::font_time());
}

// Timer callback: 5 s elapsed without user interaction → advance to runtime.
static void complete_timer_cb(lv_timer_t* t) {
    SetupState* s = static_cast<SetupState*>(lv_timer_get_user_data(t));
    if (s) s->complete_timer = nullptr;
    LOG("[setup] Complete timer fired — transitioning to runtime\n");
    state_machine::on_setup_complete();
}

void build_complete(lv_obj_t* content, SetupState* s, lv_obj_t* screen) {
    // Persist "setup done" the instant the Complete screen appears — NOT only
    // after the 5 s linger / button. Otherwise a power cycle while this screen
    // is showing would still read provisioned=false + awaiting_phone=true and
    // resume to Step 4 ("Connect on iPhone or iPad") — confusing, since pairing is
    // already done. Marking here makes a power cycle boot straight to runtime.
    // (on_setup_complete() also calls this later; it's idempotent.)
    nvs::mark_setup_complete();

    // mark_setup_complete() just flipped is_first_boot() to false, so the state
    // machine would otherwise rebuild to the runtime screen on the next tick and
    // cut the checkmark animation short. Hold the screen until the 5 s timer
    // (complete_timer_cb → on_setup_complete) hands off to runtime.
    state_machine::hold_for_setup_complete();

    ui::make_brand_mark(content);

    // Flex-grow block: checkmark + text centred in the remaining height.
    lv_obj_t* mid = make_mid(content);

    make_ok_check(mid);

    // Greet by the FIRST name from the profile Orion synced (g_name is populated
    // by run_staged_commit before this Complete screen builds). Use the first
    // whitespace-delimited token; fall back to a plain "Welcome" if no name came.
    const char* full = widget_profile_card::get_profile_name();
    char greeting[128];
    if (full && full[0]) {
        char first[97] = {};
        size_t i = 0;
        while (full[i] && full[i] != ' ' && i < sizeof(first) - 1) {
            first[i] = full[i];
            ++i;
        }
        first[i] = '\0';
        snprintf(greeting, sizeof(greeting), "Welcome, %s", first);
    } else {
        snprintf(greeting, sizeof(greeting), "Welcome");
    }
    lv_obj_t* h = lv_label_create(mid);
    lv_label_set_text(h, greeting);
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
    g_current_step = s->step;

    // Cancel any pending auto-advance timer from a previous Complete render.
    if (s->complete_timer) {
        lv_timer_delete(s->complete_timer);
        s->complete_timer = nullptr;
    }

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

    // Advertise the ANCS UUID only while the phone-pairing step is on screen.
    // This covers both Setup Step 4 and the runtime re-pair flow (both rebuild
    // through here). Leaving the step (to Complete, or any other page) closes
    // the window; dismissal-by-deletion is handled in the screen DELETE handler.
    ble_manager::set_iphone_pairing_window(s->step == Step::PhonePairing);
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
    // Skip (setup) / Close (runtime re-pair) — same exit as a successful bond.
    screen_setup::dismiss_phone_pairing(screen);
}

} // namespace

namespace screen_setup {

bool is_pairing_step_active() {
    return g_current_step == Step::Pairing;
}

lv_obj_t* create(Step initial, lv_obj_t* prev_screen) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    auto* s = new SetupState();
    s->content         = nullptr;
    s->passkey_modal   = nullptr;
    s->orioning_modal  = nullptr;
    s->orioning_ring   = nullptr;
    s->pairing_spinner = nullptr;
    s->complete_timer  = nullptr;
    s->prev_screen     = prev_screen;
    s->step            = initial;
    s->id              = ++g_next_setup_id;

    // Dot row — anchored at the fixed y so it never moves between pages.
    s->dots_row = lv_obj_create(screen);
    int16_t dot_row_width = 3 * DOT_SIZE + 2 * DOT_GAP;
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

    for (int i = 0; i < 3; ++i) {
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
        // Runtime re-pair is dismissed by deleting this screen (Skip → prev
        // screen) without a step change, so close the pairing window here too.
        //
        // Guard against a spurious top-level rebuild that tears this screen
        // down and replaces it with ANOTHER PhonePairing-step screen (e.g.
        // state_machine::evaluate() rebuilding AppState::SETUP from scratch
        // while already on Step 3/4 — see state-machine.md tick_cb notes).
        // A brand-new screen_setup::create() bumps g_next_setup_id BEFORE
        // load_screen() tears this one down (construction, including
        // rebuild_for()'s own set_iphone_pairing_window(true), runs fully
        // before the old screen is deleted). So if a newer setup screen has
        // since been created, this one's id is stale — skip closing, since
        // the newer screen's rebuild_for() already set the window correctly
        // and closing it here would just slam shut what it opened, with
        // nothing left to reopen it.
        if (ss && ss->step == screen_setup::Step::PhonePairing &&
            ss->id == g_next_setup_id) {
            ble_manager::set_iphone_pairing_window(false);
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

lv_obj_t* show_passkey_modal(lv_obj_t* screen, uint32_t passkey) {
    auto* s = static_cast<SetupState*>(lv_obj_get_user_data(screen));
    if (!s) return nullptr;
    if (s->passkey_modal) return s->passkey_modal;

    // Spinner is invisible under the scrim — pause its timer too, not just
    // hide it, so it actually stops costing CPU (a hidden lv_obj still ticks
    // its own timer and calls lv_arc_set_rotation() every 42 ms otherwise).
    if (s->pairing_spinner) {
        lv_obj_add_flag(s->pairing_spinner, LV_OBJ_FLAG_HIDDEN);
        auto* ss = static_cast<SpinnerState*>(lv_obj_get_user_data(s->pairing_spinner));
        if (ss) lv_timer_pause(ss->timer);
    }

    lv_obj_t* scrim;
    lv_obj_t* card = make_step_modal_card(screen, /*absorb_taps=*/true, &scrim);

    lv_obj_t* h = lv_label_create(card);
    lv_label_set_text(h, "Confirm on Orion");
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_h2(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);

    char passkey_str[8];
    snprintf(passkey_str, sizeof(passkey_str), "%06u", (unsigned)passkey);
    lv_obj_t* digits = lv_label_create(card);
    lv_label_set_text(digits, passkey_str);
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
    if (s->pairing_spinner) {
        lv_obj_clear_flag(s->pairing_spinner, LV_OBJ_FLAG_HIDDEN);
        auto* ss = static_cast<SpinnerState*>(lv_obj_get_user_data(s->pairing_spinner));
        if (ss) lv_timer_resume(ss->timer);
    }
}

void dismiss_phone_pairing(lv_obj_t* screen) {
    auto* s = static_cast<SetupState*>(lv_obj_get_user_data(screen));
    if (!s) return;
    hide_passkey_modal(screen);  // drop the passkey overlay if it's still up
    if (s->prev_screen) {
        // Runtime re-pair done/cancelled: return to the launching screen.
        // auto_del=true deletes this PhonePairing screen (and the passkey scrim
        // that lives on it); prev_screen stays (launch site used auto_del=false).
        lv_scr_load_anim(s->prev_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, /*auto_del=*/true);
    } else {
        // First-time setup: advance to the Complete step.
        set_step(screen, Step::Complete);
    }
}

lv_obj_t* show_orioning_modal(lv_obj_t* screen) {
    auto* s = static_cast<SetupState*>(lv_obj_get_user_data(screen));
    if (!s) return nullptr;
    if (s->orioning_modal) return s->orioning_modal;

    lv_obj_t* scrim;
    lv_obj_t* card = make_step_modal_card(screen, /*absorb_taps=*/false, &scrim);

    lv_obj_t* heading = lv_label_create(card);
    lv_label_set_text(heading, "A busy day ahead\xe2\x80\xa6");
    lv_obj_set_style_text_color(heading, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(heading, theme::font_title(), 0);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* ring = widget_progress_ring::create(card, 140, 8);
    lv_obj_set_style_pad_top(ring, 24, 0);
    widget_progress_ring::set_value(ring, 0);
    widget_progress_ring::set_label_font(ring, theme::font_time());
    widget_progress_ring::set_label_text_center(ring, "0%", -8);

    s->orioning_ring  = ring;
    s->orioning_modal = scrim;
    lv_obj_move_foreground(scrim);
    return scrim;
}

void hide_orioning_modal(lv_obj_t* screen) {
    auto* s = static_cast<SetupState*>(lv_obj_get_user_data(screen));
    if (!s || !s->orioning_modal) return;
    lv_obj_delete(s->orioning_modal);
    s->orioning_modal = nullptr;
    s->orioning_ring  = nullptr;
}

void update_orioning_progress(lv_obj_t* screen, uint8_t pct) {
    auto* s = static_cast<SetupState*>(lv_obj_get_user_data(screen));
    if (!s || !s->orioning_ring) return;
    if (pct > 100) pct = 100;
    widget_progress_ring::set_value(s->orioning_ring, pct);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    widget_progress_ring::set_label_text_center(s->orioning_ring, buf, -8);
}

} // namespace screen_setup
