#include "screens/modal_incoming_call.h"

#include <lvgl.h>

#include "app_state.h"
#include "ble/ancs_client.h"
#include "theme.h"
#include "ui_helpers.h"

// Incoming-call banner. Layout mirrors the ANCS detail overlay (content on a
// dark scrim) with a call-specific eyebrow + caller name and Decline / Dismiss.

namespace {

struct CallCtx {
    lv_obj_t* scrim;
    uint32_t  uid;
};

// The single open banner (only one call banner at a time — latest wins).
lv_obj_t* g_call_scrim = nullptr;
uint32_t  g_call_uid   = 0;

void on_decline(lv_event_t* e) {
    auto* ctx = static_cast<CallCtx*>(lv_event_get_user_data(e));
    // ANCS Negative action on an IncomingCall = Decline. dismiss_notification()
    // sends it and removes the notification from Ori's queue. UID 0 is valid, so
    // decline unconditionally.
    ancs_client::dismiss_notification(ctx->uid);
    lv_obj_delete(ctx->scrim);
}

void on_dismiss(lv_event_t* e) {
    auto* ctx = static_cast<CallCtx*>(lv_event_get_user_data(e));
    lv_obj_delete(ctx->scrim);  // hide banner only — call keeps ringing
}

} // namespace

namespace modal_incoming_call {

void show(uint32_t uid) {
    // Replace any banner already up (latest call wins).
    if (g_call_scrim) lv_obj_delete(g_call_scrim);

    const app_state::AncsNotification& n = app_state::ancs_notification_by_uid(uid);

    auto* ctx = new CallCtx();
    ui::ModalLayout layout = ui::make_modal_layout(lv_screen_active(), 660, 380);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* card        = layout.card;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    ctx->scrim   = scrim;
    ctx->uid     = uid;
    g_call_scrim = scrim;
    g_call_uid   = uid;
    lv_obj_add_event_cb(scrim, [](lv_event_t* e) {
        auto* c = static_cast<CallCtx*>(lv_event_get_user_data(e));
        if (g_call_scrim == c->scrim) { g_call_scrim = nullptr; g_call_uid = 0; }
        delete c;
    }, LV_EVENT_DELETE, ctx);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t*) {}, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sp_top = lv_obj_create(scroll_area);
    ui::clear_container(sp_top);
    lv_obj_set_size(sp_top, 0, 0);
    lv_obj_set_flex_grow(sp_top, 1);

    // Eyebrow.
    lv_obj_t* eyebrow = lv_label_create(scroll_area);
    lv_label_set_text(eyebrow, "INCOMING CALL");
    lv_obj_set_style_text_font(eyebrow, theme::font_meta(), 0);
    lv_obj_set_style_text_color(eyebrow, theme::color(theme::COLOR_ACCENT), 0);
    lv_obj_set_style_text_align(eyebrow, LV_TEXT_ALIGN_CENTER, 0);

    // Caller (notification title).
    lv_obj_t* caller = lv_label_create(scroll_area);
    lv_label_set_long_mode(caller, LV_LABEL_LONG_WRAP);
    lv_label_set_text(caller, (n.title && n.title[0]) ? n.title : "Unknown caller");
    lv_obj_set_width(caller, lv_pct(100));
    lv_obj_set_style_text_font(caller, theme::font_h2(), 0);
    lv_obj_set_style_text_color(caller, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(caller, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(caller, 8, 0);

    // Secondary line: notification body if present, else the app name.
    const char* sub = (n.body && n.body[0]) ? n.body : n.display_name;
    if (sub && sub[0]) {
        lv_obj_t* app = lv_label_create(scroll_area);
        lv_label_set_text(app, sub);
        lv_obj_set_style_text_font(app, theme::font_meta(), 0);
        lv_obj_set_style_text_color(app, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_align(app, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(app, 6, 0);
    }

    lv_obj_t* sp_bot = lv_obj_create(scroll_area);
    ui::clear_container(sp_bot);
    lv_obj_set_size(sp_bot, 0, 0);
    lv_obj_set_flex_grow(sp_bot, 1);

    lv_obj_t* decline = ui::make_btn(actions, "Decline", ui::BtnStyle::Danger,
                                     nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(decline, on_decline, LV_EVENT_CLICKED, ctx);

    lv_obj_t* dismiss = ui::make_btn(actions, "Dismiss", ui::BtnStyle::Tertiary,
                                     nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(dismiss, on_dismiss, LV_EVENT_CLICKED, ctx);
}

void close_if_showing(uint32_t uid) {
    // UID 0 is a valid ANCS UID — match on the open scrim + UID.
    if (g_call_scrim && g_call_uid == uid) lv_obj_delete(g_call_scrim);
}

} // namespace modal_incoming_call
