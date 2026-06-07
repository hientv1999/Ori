#include "screens/modal_ancs_notification.h"

#include <lvgl.h>

#include "app_state.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_status_bar.h"

// ANCS notification detail modal.
//
// Layout mirrors the meeting-detail overlay in screen_meeting_list.cpp:
// content sits directly on the dark scrim (no card box), centred, with the
// same font-size and colour hierarchy:
//
//   notification title            ← font_title (24px), md-title equivalent
//   message body                  ← font_meta  (22px), secondary, wrapping
//   timestamp                     ← font_meta  (22px), tertiary (md-org)
//   app name                      ← font_h2    (28px), primary  (md-time)
//   [Read]  [Close]               ← replaces "Tap to dismiss" hint

namespace {

struct ModalCtx {
    lv_obj_t*   scrim;
    lv_obj_t*   ancs_tile;   // tile in the status bar
    const char* token;       // app token — used to remove from queue on "Read"
};

void on_read(lv_event_t* e) {
    auto* ctx = static_cast<ModalCtx*>(lv_event_get_user_data(e));
    // Remove from queue and refresh — reveals the next queued icon if one exists.
    app_state::dismiss_ancs_notification(ctx->token);
    if (ctx->ancs_tile) {
        lv_obj_t* ancs_row = lv_obj_get_parent(ctx->ancs_tile);
        lv_obj_t* bar      = ancs_row ? lv_obj_get_parent(ancs_row) : nullptr;
        if (bar) widget_status_bar::refresh(bar);
    }
    lv_obj_delete(ctx->scrim);
    // ctx is freed by the scrim's LV_EVENT_DELETE handler below.
}

void on_close(lv_event_t* e) {
    auto* ctx = static_cast<ModalCtx*>(lv_event_get_user_data(e));
    lv_obj_delete(ctx->scrim);
}

} // namespace

namespace modal_ancs_notification {

lv_obj_t* create(lv_obj_t* base_screen, lv_obj_t* ancs_tile, const char* token) {
    const app_state::AncsNotification& n = app_state::ancs_notification(token);

    auto* ctx = new ModalCtx();

    ui::ModalLayout layout = ui::make_modal_layout(base_screen, 660, 400);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* card        = layout.card;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    ctx->scrim     = scrim;
    ctx->ancs_tile = ancs_tile;
    ctx->token     = token;
    lv_obj_add_event_cb(scrim, [](lv_event_t* e) {
        delete static_cast<ModalCtx*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, ctx);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t*) {}, LV_EVENT_CLICKED, nullptr);
    // Top spacer — centres content when it is shorter than scroll_area's height.
    lv_obj_t* sa_spacer_top = lv_obj_create(scroll_area);
    ui::clear_container(sa_spacer_top);
    lv_obj_set_size(sa_spacer_top, 0, 0);
    lv_obj_set_flex_grow(sa_spacer_top, 1);

    // ── Notification title (font_title = 24px, md-title equivalent) ──
    lv_obj_t* title = lv_label_create(scroll_area);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title, n.title);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, theme::font_title(), 0);
    lv_obj_set_style_text_color(title, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(title, 8, 0);

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

    lv_obj_t* read_btn = ui::make_btn(actions, "Read", ui::BtnStyle::Primary,
                                      nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(read_btn, on_read, LV_EVENT_CLICKED, ctx);

    lv_obj_t* close_btn = ui::make_btn(actions, "Close", ui::BtnStyle::Tertiary,
                                       nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(close_btn, on_close, LV_EVENT_CLICKED, ctx);

    return scrim;
}

} // namespace modal_ancs_notification
