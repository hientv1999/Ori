#include "screens/screen_reconnect_syncing.h"

#include <cstdio>
#include <lvgl.h>

#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_progress_ring.h"
#include "widgets/widget_status_bar.h"

// Reconnect-Syncing overlay (state-machine.md §Reconnect-Syncing).
//
// IMPORTANT: status bar + profile card remain visible. Only the left panel
// is masked by the syncing overlay. The overlay reuses the Step 3
// "Orioning" ring at a smaller diameter to fit the 528 x 396 left panel
// cleanly, plus two text lines.
//
// Ring is determinate (0–100 %) driven by OrioningProgress BLE events,
// matching the Orioning modal style on first-time setup.

namespace screen_reconnect_syncing {

// Ring widget reference — cleared when the screen is destroyed.
static lv_obj_t* g_ring = nullptr;

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    widget_status_bar::create(screen);

    lv_obj_t* body = ui::make_screen_body(screen);

    // Left panel — the masked area.
    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, 528, lv_pct(100));
    lv_obj_set_style_bg_color(left, theme::color(theme::COLOR_BG), 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    // Block all touch — overlay is non-dismissable.
    lv_obj_add_flag(left, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* ring = widget_progress_ring::create(left, 160, 8);
    lv_obj_set_style_pad_top(ring, 18, 0);
    widget_progress_ring::set_value(ring, 0);
    widget_progress_ring::set_label_font(ring, theme::font_time());
    widget_progress_ring::set_label_text_center(ring, "0%", -8);
    g_ring = ring;

    // Clear module-level reference when this screen is deleted.
    lv_obj_add_event_cb(screen, [](lv_event_t*) { g_ring = nullptr; },
                        LV_EVENT_DELETE, nullptr);

    // Subtitle: "Refreshing your day…"
    lv_obj_t* sub = lv_label_create(left);
    lv_label_set_text_static(sub, "Refreshing your day\xe2\x80\xa6");
    lv_obj_set_style_text_color(sub, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(sub, theme::font_title(), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(sub, 14, 0);

    ui::make_panel_divider(body);
    widget_profile_card::create(body);
    return screen;
}

void set_progress(uint8_t pct) {
    if (!g_ring) return;
    if (pct > 100) pct = 100;
    widget_progress_ring::set_value(g_ring, pct);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    widget_progress_ring::set_label_text_center(g_ring, buf, -8);
}

} // namespace screen_reconnect_syncing
