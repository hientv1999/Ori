#include "screens/screen_ota_updating.h"

#include <lvgl.h>

#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_progress_ring.h"

// OTA-Updating full-screen takeover.
//
// status bar HIDDEN, profile card HIDDEN, left panel HIDDEN — only the
// "Updating firmware… N%" page is visible per ota.md.
//
// All touch is inert (a transparent clickable layer covers the entire
// screen and absorbs presses without doing anything).
//
// M5: mock timer removed. Progress is driven by ota_receiver::poll()
// calling screen_ota_updating::set_progress() on each PROGRESS frame.

namespace {

struct OtaState {
    lv_obj_t* ring;
    lv_obj_t* percent_label;
    lv_obj_t* heading_label;
    lv_obj_t* sub_label;
};

// Module-level pointer to the live OTA state, so set_progress() can reach it.
OtaState* g_ota_ui = nullptr;

void on_screen_delete(lv_event_t* e) {
    auto* s = static_cast<OtaState*>(lv_event_get_user_data(e));
    if (s == g_ota_ui) g_ota_ui = nullptr;
    delete s;
}

} // namespace

namespace screen_ota_updating {

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    auto* state = new OtaState();
    state->ring = nullptr;
    state->percent_label = nullptr;
    state->heading_label = nullptr;
    state->sub_label = nullptr;
    g_ota_ui = state;

    lv_obj_t* root = lv_obj_create(screen);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    // Absorb all touch — non-dismissable.
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    state->heading_label = lv_label_create(root);
    lv_label_set_text_static(state->heading_label, "Updating firmware");
    lv_obj_set_style_text_color(state->heading_label,
        theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(state->heading_label, theme::font_display(), 0);
    lv_obj_set_style_text_align(state->heading_label, LV_TEXT_ALIGN_CENTER, 0);

    state->ring = widget_progress_ring::create(root, 220, 8);
    lv_obj_set_style_pad_top(state->ring, 30, 0);
    widget_progress_ring::set_value(state->ring, 0);
    widget_progress_ring::set_label_text_center(state->ring, "0%", -8);

    // Download phase: image streams into PSRAM and the ring advances live.
    lv_obj_t* sub = lv_label_create(root);
    state->sub_label = sub;
    lv_label_set_text_static(sub, "Keep Ori plugged in");
    // Match "Your desk deserves better" on Welcome — secondary, 26 px.
    lv_obj_set_style_text_color(sub, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(sub, theme::font_title(), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(sub, 24, 0);

    lv_obj_set_user_data(screen, state);
    lv_obj_add_event_cb(screen, on_screen_delete, LV_EVENT_DELETE, state);

    return screen;
}

void set_progress(uint8_t pct) {
    if (!g_ota_ui || !g_ota_ui->ring) return;
    if (pct > 100) pct = 100;
    widget_progress_ring::set_value(g_ota_ui->ring, pct);
    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%d%%", pct);
    widget_progress_ring::set_label_text_center(g_ota_ui->ring, buf, -8);
}

void set_installing(uint32_t linger_ms) {
    // Download done; the firmware is about to be written to flash. This is the
    // last frame shown before the panel goes dark (the flash phase has to halt
    // the LCD — PSRAM-DMA vs flash bus contention), so it must say so.
    if (!g_ota_ui || !g_ota_ui->heading_label) return;
    lv_obj_t* root   = lv_obj_get_parent(g_ota_ui->heading_label);
    lv_obj_t* screen = lv_obj_get_parent(root);

    // Keep the title at the SAME Y as the "Updating firmware" title so the
    // in-place Updating→Installing transition doesn't shift it. Capture the
    // title's laid-out position, then pin it there out of the flex flow (before
    // hiding the ring, so the later re-layout can't move it).
    lv_obj_update_layout(screen);
    int32_t title_y = lv_obj_get_y(g_ota_ui->heading_label);
    lv_label_set_text_static(g_ota_ui->heading_label, "Installing firmware");
    lv_obj_add_flag(g_ota_ui->heading_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(g_ota_ui->heading_label, LV_ALIGN_TOP_MID, 0, title_y);

    // Hide the progress ring — there's no live percentage during the flash commit.
    if (g_ota_ui->ring)
        lv_obj_add_flag(g_ota_ui->ring, LV_OBJ_FLAG_HIDDEN);

    if (g_ota_ui->sub_label) {
        lv_label_set_text_static(g_ota_ui->sub_label,
            "Screen goes dark for a few seconds —\n"
            "keep Ori plugged in. It restarts when done.");
        // Take the instruction text out of the (top-aligned) flex flow and pin it
        // to the centre of the screen, independent of the title at the top.
        lv_obj_add_flag(g_ota_ui->sub_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_style_pad_top(g_ota_ui->sub_label, 0, 0);
        lv_obj_align(g_ota_ui->sub_label, LV_ALIGN_CENTER, 0, 0);
    }

    // Countdown bar — 6 px accent strip at the very bottom that fills over the
    // linger window, so the user sees how long until the screen goes dark.
    // Mirrors the Setup-Complete countdown bar (screen_setup.cpp). Created on the
    // top-level screen so it sits above the full-screen root; cleaned up with the
    // screen on reboot/screen-swap.
    lv_obj_t* bar = lv_bar_create(screen);
    lv_obj_set_size(bar, 800, 6);   // 2x the 3 px Setup-Complete bar
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
    lv_anim_set_time(&a, linger_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

// ── Shared builders for the button OTA screens ──────────────────────────────

// 800×480 top-aligned column. button_screen=true reserves bottom room so the
// (last-child) button lands near the bottom, matching the setup primary button.
static lv_obj_t* make_base(bool button_screen) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    lv_obj_t* root = lv_obj_create(screen);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_top(root, 30, 0);
    lv_obj_set_style_pad_bottom(root, button_screen ? 70 : 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);  // absorb stray touches
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return root;
}

// Flex-grow block that vertically centres its children between the top content
// and the bottom button.
static lv_obj_t* make_mid(lv_obj_t* parent) {
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

static lv_obj_t* make_title(lv_obj_t* parent, const char* text) {
    lv_obj_t* h = lv_label_create(parent);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(h, theme::font_display(), 0);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    return h;
}

// Subtext style — matches "Your desk deserves better" on the Welcome screen
// (secondary colour, 26 px) for visibility + consistency across the flow.
static lv_obj_t* make_sub(lv_obj_t* parent, const char* text) {
    lv_obj_t* p = lv_label_create(parent);
    lv_label_set_text(p, text);
    lv_obj_set_style_text_color(p, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(p, theme::font_title(), 0);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    return p;
}

static lv_obj_t* make_glyph_circle(lv_obj_t* parent, uint32_t bg, uint32_t border) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, 132, 132);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, theme::color(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, theme::color(border), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

// Warning glyph — danger circle with a bold "!".
static lv_obj_t* make_warn_glyph(lv_obj_t* parent) {
    lv_obj_t* c = make_glyph_circle(parent, theme::COLOR_DANGER_SOFT, theme::COLOR_DANGER);
    lv_obj_t* bang = lv_label_create(c);
    lv_label_set_text(bang, "!");
    lv_obj_set_style_text_color(bang, theme::color(theme::COLOR_DANGER), 0);
    lv_obj_set_style_text_font(bang, theme::font_large(), 0);
    lv_obj_center(bang);
    return c;
}

// Animated "OK" check — mirrors make_ok_check() on the Setup-complete screen.
static void check_ring_anim_cb(void* obj, int32_t val) {
    lv_arc_set_value((lv_obj_t*)obj, val);
}
static void check_reveal_tick_cb(lv_timer_t* t) {
    lv_obj_t* tick = (lv_obj_t*)lv_timer_get_user_data(t);
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_HIDDEN);
    // This timer has repeat_count=1, so LVGL auto-deletes it right after this
    // callback returns (lv_timer.c) — `t` is dangling the instant we return.
    // `tick`'s own DELETE handler below still holds that now-stale pointer in
    // its user_data and would call lv_timer_delete() on it a second time when
    // the screen is torn down later, corrupting LVGL's timer list. Null it out
    // here so that handler becomes a no-op — mirrors screen_setup.cpp's
    // reveal_tick_cb(), which this was copied from but was missing this line.
    lv_obj_set_user_data(tick, nullptr);
}
static lv_obj_t* make_ok_check(lv_obj_t* parent) {
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, 120, 120);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_color(root, theme::color(theme::COLOR_OK), 0);
    lv_obj_set_style_shadow_width(root, 22, 0);
    lv_obj_set_style_shadow_opa(root, LV_OPA_40, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* arc = lv_arc_create(root);
    lv_obj_set_size(arc, 120, 120);
    lv_obj_center(arc);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_value(arc, 0);
    lv_arc_set_rotation(arc, 270);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(arc, theme::color(theme::COLOR_OK), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, theme::color(theme::COLOR_OK), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

    static lv_point_precise_t tick_pts[3] = {{25, 60}, {48, 80}, {88, 40}};
    lv_obj_t* tick = lv_line_create(root);
    lv_line_set_points(tick, tick_pts, 3);
    lv_obj_set_style_line_color(tick, theme::color(theme::COLOR_OK), 0);
    lv_obj_set_style_line_width(tick, 6, 0);
    lv_obj_set_style_line_rounded(tick, true, 0);
    lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, check_ring_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_timer_t* t = lv_timer_create(check_reveal_tick_cb, 800, tick);
    lv_timer_set_repeat_count(t, 1);

    // If `tick` is deleted before the timer fires (e.g. the user taps Close
    // on create_updated_ack() well within 800 ms), delete the pending timer
    // too so it never runs against a freed object — mirrors the same guard
    // on the Setup-complete screen's make_ok_check() (screen_setup.cpp).
    lv_obj_set_user_data(tick, t);
    lv_obj_add_event_cb(tick, [](lv_event_t* e) {
        auto* timer = static_cast<lv_timer_t*>(
            lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e))));
        if (timer) lv_timer_delete(timer);
    }, LV_EVENT_DELETE, nullptr);

    return root;
}

// ── User-gated flow screens ─────────────────────────────────────────────────

lv_obj_t* create_updated_ack(const char* version, lv_event_cb_t on_close) {
    lv_obj_t* root  = make_base(/*button_screen=*/true);
    lv_obj_t* title = make_title(root, "Firmware updated");
    lv_obj_set_style_pad_top(title, 36, 0);
    lv_obj_t* mid = make_mid(root);
    char buf[72];
    lv_snprintf(buf, sizeof(buf), "Ori is now running version %s", version ? version : "");
    make_sub(mid, buf);
    lv_obj_t* chk = make_ok_check(mid);
    lv_obj_set_style_margin_top(chk, 24, 0);
    ui::make_btn(root, "Close", ui::BtnStyle::Tertiary, on_close, nullptr,
                 16, 40, theme::font_title());
    return lv_obj_get_parent(root);
}

lv_obj_t* create_error(const char* message, lv_event_cb_t on_close) {
    lv_obj_t* root = make_base(/*button_screen=*/true);
    lv_obj_t* mid  = make_mid(root);
    make_title(mid, "Update failed");
    lv_obj_t* g = make_warn_glyph(mid);
    lv_obj_set_style_margin_top(g, 30, 0);
    lv_obj_t* sub = make_sub(mid,
        message ? message : "The update couldn't be installed — try again from Orion");
    lv_obj_set_width(sub, 600);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(sub, 24, 0);
    ui::make_btn(root, "Close", ui::BtnStyle::Tertiary, on_close, nullptr,
                 16, 40, theme::font_title());
    return lv_obj_get_parent(root);
}

} // namespace screen_ota_updating
