#include "screens/modal_ancs_notification.h"

#include <lvgl.h>

#include "assets/ancs_icons.h"
#include "mock_data.h"
#include "theme.h"
#include "ui_helpers.h"

// ANCS notification detail modal.
//
// Layout mirrors the meeting-detail overlay in screen_meeting_list.cpp:
// content sits directly on the dark scrim (no card box), centred, with the
// same font-size and colour hierarchy:
//
//   [80×80 app icon circle]       ← solid-colour circle from ancs_icons
//   notification title            ← font_title (24px), md-title equivalent
//   message body                  ← font_meta  (22px), secondary, wrapping
//   timestamp                     ← font_meta  (22px), tertiary (md-org)
//   app name                      ← font_h2    (28px), primary  (md-time)
//   [Read]  [Close]               ← replaces "Tap to dismiss" hint

namespace {

struct ModalCtx {
    lv_obj_t*   scrim;
    lv_obj_t*   ancs_tile;   // tile in the status bar — hidden on "Read"
};

void on_read(lv_event_t* e) {
    auto* ctx = static_cast<ModalCtx*>(lv_event_get_user_data(e));
    if (ctx->ancs_tile) lv_obj_add_flag(ctx->ancs_tile, LV_OBJ_FLAG_HIDDEN);
    lv_obj_del(ctx->scrim);
    // ctx is freed by the scrim's LV_EVENT_DELETE handler below.
}

void on_close(lv_event_t* e) {
    auto* ctx = static_cast<ModalCtx*>(lv_event_get_user_data(e));
    lv_obj_del(ctx->scrim);
}

} // namespace

namespace modal_ancs_notification {

lv_obj_t* create(lv_obj_t* base_screen, lv_obj_t* ancs_tile, const char* token) {
    const mock_data::AncsNotification& n = mock_data::ancs_notification(token);

    auto* ctx = new ModalCtx();

    // Full-screen scrim — does NOT dismiss on tap (card has action buttons).
    lv_obj_t* scrim = lv_obj_create(base_screen);
    ctx->scrim     = scrim;
    ctx->ancs_tile = ancs_tile;
    lv_obj_set_size(scrim, 800, 480);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, theme::color(theme::COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, theme::SCRIM_OPA, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);  // absorbs taps outside box
    lv_obj_add_event_cb(scrim, [](lv_event_t* e) {
        delete static_cast<ModalCtx*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, ctx);

    // Outer box — full screen. scroll_area grows to fill space above actions.
    // OVERFLOW_VISIBLE lets button glow bleed beyond the box bounds.
    lv_obj_t* box = lv_obj_create(scrim);
    lv_obj_set_size(box, 800, 480);
    lv_obj_set_pos(box, 0, 0);
    ui::clear_container(box);
    lv_obj_set_style_pad_left(box, 40, 0);
    lv_obj_set_style_pad_right(box, 40, 0);
    lv_obj_set_style_pad_top(box, 24, 0);
    lv_obj_set_style_pad_bottom(box, 24, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, [](lv_event_t*) {}, LV_EVENT_CLICKED, nullptr);

    // Scrollable text area — grows to fill all vertical space above the
    // action buttons via flex_grow(1). The action row stays outside this
    // container so the button glow is not clipped by the scroll clip rect.
    lv_obj_t* scroll_area = lv_obj_create(box);
    lv_obj_set_width(scroll_area, lv_pct(100));
    lv_obj_set_flex_grow(scroll_area, 1);
    lv_obj_set_style_bg_opa(scroll_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_area, 0, 0);
    lv_obj_set_style_pad_all(scroll_area, 0, 0);
    lv_obj_add_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(scroll_area, LV_DIR_VER);
    lv_obj_set_flex_flow(scroll_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Top spacer — centres content when it is shorter than scroll_area's height.
    lv_obj_t* sa_spacer_top = lv_obj_create(scroll_area);
    ui::clear_container(sa_spacer_top);
    lv_obj_set_size(sa_spacer_top, 0, 0);
    lv_obj_set_flex_grow(sa_spacer_top, 1);

    // ── App icon circle (80×80, brand colour from ancs_icons) ──
    lv_obj_t* icon = lv_obj_create(scroll_area);
    lv_obj_set_size(icon, 80, 80);
    lv_obj_set_style_bg_color(icon, theme::color(ancs_icons::color(token)), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    // ── Notification title (font_title = 24px, md-title equivalent) ──
    lv_obj_t* title = lv_label_create(scroll_area);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title, n.title);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, theme::font_title(), 0);
    lv_obj_set_style_text_color(title, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(title, 16, 0);

    // ── Message body (font_meta = 22px, secondary — md-loc equivalent) ──
    lv_obj_t* body = lv_label_create(scroll_area);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body, n.body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_font(body, theme::font_meta(), 0);
    lv_obj_set_style_text_color(body, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(body, 16, 0);

    // ── Timestamp (font_meta = 22px, tertiary — md-org equivalent) ──
    lv_obj_t* time_lbl = lv_label_create(scroll_area);
    lv_label_set_text(time_lbl, n.time_ago);
    lv_obj_set_style_text_font(time_lbl, theme::font_meta(), 0);
    lv_obj_set_style_text_color(time_lbl, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(time_lbl, 4, 0);

    // ── App name (font_h2 = 28px, primary — md-time equivalent, anchor id) ──
    lv_obj_t* app_name = lv_label_create(scroll_area);
    lv_label_set_text(app_name, n.display_name);
    lv_obj_set_style_text_font(app_name, theme::font_h2(), 0);
    lv_obj_set_style_text_color(app_name, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(app_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(app_name, 12, 0);

    // Bottom spacer — mirrors top spacer to keep content vertically centred.
    lv_obj_t* sa_spacer_bot = lv_obj_create(scroll_area);
    ui::clear_container(sa_spacer_bot);
    lv_obj_set_size(sa_spacer_bot, 0, 0);
    lv_obj_set_flex_grow(sa_spacer_bot, 1);

    // ── Action row — direct child of box, NOT inside scroll_area ──
    // Keeping buttons outside the scroll clip rect preserves the full
    // button glow (OVERFLOW_VISIBLE on both box and actions).
    lv_obj_t* actions = lv_obj_create(box);
    lv_obj_set_size(actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_top(actions, 28, 0);
    lv_obj_set_style_pad_column(actions, 14, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(actions, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* read_btn = ui::make_btn(actions, "Read", ui::BtnStyle::Primary,
                                      nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(read_btn, on_read, LV_EVENT_CLICKED, ctx);

    lv_obj_t* close_btn = ui::make_btn(actions, "Close", ui::BtnStyle::Tertiary,
                                       nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(close_btn, on_close, LV_EVENT_CLICKED, ctx);

    return scrim;
}

} // namespace modal_ancs_notification
