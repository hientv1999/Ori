#include "screens/screen_ota_updating.h"

#include <lvgl.h>

#include "theme.h"
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

    lv_obj_t* sub = lv_label_create(root);
    lv_label_set_text_static(sub, "Do not power off Ori");
    lv_obj_set_style_text_color(sub, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(sub, theme::font_meta(), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(sub, 24, 0);

    // M5: no mock timer — progress driven by ota_receiver::poll() calling
    // screen_ota_updating::set_progress() on each USB CDC PROGRESS frame.

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

} // namespace screen_ota_updating
