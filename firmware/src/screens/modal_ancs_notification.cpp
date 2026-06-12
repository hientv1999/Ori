#include "screens/modal_ancs_notification.h"

#include <lvgl.h>

#include "app_state.h"
#include "ble/ancs_client.h"
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
    lv_obj_t* scrim;
    uint32_t  uid;   // ANCS notification UID this overlay is showing
};

// The single currently-open detail overlay (only one can be open at a time —
// the scrim covers the status bar, so no second icon is tappable). Tracked so a
// remote ANCS Removed event can dismiss it via close_if_showing().
lv_obj_t* g_open_scrim = nullptr;
uint32_t  g_open_uid   = 0;

void on_read(lv_event_t* e) {
    auto* ctx = static_cast<ModalCtx*>(lv_event_get_user_data(e));
    // "Read" = clear this notification on the iPhone too (ANCS
    // PerformNotificationAction · Negative). dismiss_notification() sends that
    // and removes this exact UID from Ori's queue + detail + status bar
    // (queue_remove → refresh), re-windowing the visible icons. UID 0 is a
    // valid ANCS UID (often the oldest), so dismiss unconditionally.
    ancs_client::dismiss_notification(ctx->uid);
    lv_obj_delete(ctx->scrim);
    // ctx is freed by the scrim's LV_EVENT_DELETE handler below.
}

void on_close(lv_event_t* e) {
    auto* ctx = static_cast<ModalCtx*>(lv_event_get_user_data(e));
    lv_obj_delete(ctx->scrim);
}

} // namespace

namespace modal_ancs_notification {

lv_obj_t* create(lv_obj_t* base_screen, uint32_t uid) {
    const app_state::AncsNotification& n = app_state::ancs_notification_by_uid(uid);

    auto* ctx = new ModalCtx();

    ui::ModalLayout layout = ui::make_modal_layout(base_screen, 660, 400);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* card        = layout.card;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    ctx->scrim   = scrim;
    ctx->uid     = uid;
    g_open_scrim = scrim;
    g_open_uid   = uid;
    lv_obj_add_event_cb(scrim, [](lv_event_t* e) {
        auto* c = static_cast<ModalCtx*>(lv_event_get_user_data(e));
        if (g_open_scrim == c->scrim) { g_open_scrim = nullptr; g_open_uid = 0; }
        delete c;
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

    // ── Subtitle (ANCS Subtitle: mail subject / thread) — shown only if present ──
    if (n.subtitle && n.subtitle[0]) {
        lv_obj_t* sub = lv_label_create(scroll_area);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_label_set_text(sub, n.subtitle);
        lv_obj_set_width(sub, lv_pct(100));
        lv_obj_set_style_text_font(sub, theme::font_meta(), 0);
        lv_obj_set_style_text_color(sub, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(sub, 6, 0);
    }

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
    // Only shown when ANCS supplied a parseable Date — otherwise the line is
    // omitted entirely rather than showing a fabricated time.
    if (n.time_ago && n.time_ago[0]) {
        lv_obj_t* time_lbl = lv_label_create(scroll_area);
        lv_label_set_text(time_lbl, n.time_ago);
        lv_obj_set_style_text_font(time_lbl, theme::font_meta(), 0);
        lv_obj_set_style_text_color(time_lbl, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
        lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(time_lbl, 4, 0);
    }

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

void close_if_showing(uint32_t uid) {
    // UID 0 is a valid ANCS UID — match on the open scrim + UID, not on uid != 0.
    if (g_open_scrim && g_open_uid == uid) {
        lv_obj_delete(g_open_scrim);  // fires LV_EVENT_DELETE → clears globals + frees ctx
    }
}

} // namespace modal_ancs_notification
