#include "screens/modal_incoming_call.h"

#include <lvgl.h>

#include "app_state.h"
#include "ble/ancs_client.h"
#include "theme.h"
#include "ui_helpers.h"

// Incoming-call banner + in-call dialog.
//
//  • show()         — ringing banner: Decline (ANCS Negative) / Dismiss (hide).
//  • notify_active()— auto entry when a call goes active: starts the duration
//                     session and auto-presents the dialog once.
//  • show_active()  — explicit open (tapping the call's status-bar icon):
//                     ensures the session is running and presents the dialog.
//
// The call-duration timer lives in a module-level "session" that is INDEPENDENT
// of the dialog's visibility: it starts when the call goes active and keeps
// running while the dialog is hidden. Only the call ending (close_if_showing on
// the ANCS Removed event) or End call stops it.

namespace {

struct CallCtx {
    lv_obj_t* scrim;
    uint32_t  uid;
};

// The single open overlay (ringing banner or in-call dialog).
lv_obj_t* g_call_scrim  = nullptr;
uint32_t  g_call_uid    = 0;
bool      g_call_active = false;  // overlay is the in-call dialog (vs ringing banner)

// Persistent active-call session. Survives the dialog being hidden/reopened —
// the timer runs for the whole call, not just while the dialog is on screen.
struct CallSession {
    bool        running;
    uint32_t    uid;
    uint32_t    elapsed_s;   // seconds since the call went active
    lv_timer_t* tick;        // 1 s timer for the whole call (nullptr when stopped)
    lv_obj_t*   label;       // visible mm:ss label while the dialog is open; else nullptr
    bool        auto_shown;  // dialog has been auto-presented once for this call
};
CallSession g_session = {};

void fmt_duration(char* buf, size_t n, uint32_t s) {
    uint32_t hh = s / 3600, mm = (s % 3600) / 60, ss = s % 60;
    if (hh) lv_snprintf(buf, n, "%u:%02u:%02u", (unsigned)hh, (unsigned)mm, (unsigned)ss);
    else    lv_snprintf(buf, n, "%02u:%02u", (unsigned)mm, (unsigned)ss);
}

// Whole-call duration tick. Counts ticks (not the wall clock) so it doesn't
// depend on the clock being synced. Updates the label only while it's visible.
void session_tick(lv_timer_t*) {
    if (!g_session.running) return;
    g_session.elapsed_s++;
    if (g_session.label) {
        char buf[16];
        fmt_duration(buf, sizeof(buf), g_session.elapsed_s);
        lv_label_set_text(g_session.label, buf);
    }
}

// Begin a fresh session for `uid`. No-op if one is already running for it, so a
// reopen or a repeat ANCS event never resets the elapsed time.
void session_start(uint32_t uid) {
    if (g_session.running && g_session.uid == uid) return;
    if (g_session.tick) lv_timer_delete(g_session.tick);
    g_session = {};
    g_session.running = true;
    g_session.uid     = uid;
    g_session.tick    = lv_timer_create(session_tick, 1000, nullptr);
}

void session_stop() {
    if (g_session.tick) lv_timer_delete(g_session.tick);
    g_session = {};
}

// Scrim DELETE: clear the open-overlay singletons. If the in-call dialog (which
// owns the session's visible label) is the one closing, detach the label — but
// DO NOT stop the session: hiding the dialog must not stop the call timer.
void on_scrim_delete(lv_event_t* e) {
    auto* c = static_cast<CallCtx*>(lv_event_get_user_data(e));
    if (g_call_scrim == c->scrim) {
        if (g_call_active) g_session.label = nullptr;
        g_call_scrim = nullptr; g_call_uid = 0; g_call_active = false;
    }
    delete c;
}

void on_decline(lv_event_t* e) {
    auto* ctx = static_cast<CallCtx*>(lv_event_get_user_data(e));
    // ANCS Negative on an IncomingCall = Decline. UID 0 is valid, so unconditional.
    ancs_client::dismiss_notification(ctx->uid);
    lv_obj_delete(ctx->scrim);
}

// Answer a ringing call: ANCS Positive action, then switch to the in-call dialog
// (which starts the duration timer). show_active() replaces this banner.
void on_answer(lv_event_t* e) {
    auto* ctx = static_cast<CallCtx*>(lv_event_get_user_data(e));
    uint32_t uid = ctx->uid;
    ancs_client::answer_notification(uid);
    modal_incoming_call::show_active(uid);
}

// End an active call: same ANCS Negative action (= hang up). Stops the timer now
// rather than waiting for the ANCS Removed event.
void on_end_call(lv_event_t* e) {
    auto* ctx = static_cast<CallCtx*>(lv_event_get_user_data(e));
    session_stop();
    ancs_client::dismiss_notification(ctx->uid);
    lv_obj_delete(ctx->scrim);
}

void on_dismiss(lv_event_t* e) {
    auto* ctx = static_cast<CallCtx*>(lv_event_get_user_data(e));
    lv_obj_delete(ctx->scrim);  // hide overlay only — the call (and its timer) continues
}

// Build the in-call dialog for `uid` and bind it to the running session. The
// caller must have started the session first (session_start).
void present_active_dialog(uint32_t uid) {
    // Already showing the dialog for this call → keep it (and the live label).
    if (g_call_scrim && g_call_active && g_call_uid == uid) return;
    if (g_call_scrim) lv_obj_delete(g_call_scrim);  // replace ringing banner / other

    const app_state::AncsNotification& n = app_state::ancs_notification_by_uid(uid);

    auto* ctx = new CallCtx();
    ui::ModalLayout layout = ui::make_modal_layout(lv_screen_active(), 660, 380);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* card        = layout.card;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    ctx->scrim    = scrim;
    ctx->uid      = uid;
    g_call_scrim  = scrim;
    g_call_uid    = uid;
    g_call_active = true;
    lv_obj_add_event_cb(scrim, on_scrim_delete, LV_EVENT_DELETE, ctx);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t*) {}, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* sp_top = lv_obj_create(scroll_area);
    ui::clear_container(sp_top);
    lv_obj_set_size(sp_top, 0, 0);
    lv_obj_set_flex_grow(sp_top, 1);

    // Eyebrow.
    lv_obj_t* eyebrow = lv_label_create(scroll_area);
    lv_label_set_text(eyebrow, "ON CALL");
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

    // Live call-duration timer — bound to the persistent session below.
    lv_obj_t* timer_lbl = lv_label_create(scroll_area);
    lv_obj_set_style_text_font(timer_lbl, theme::font_display(), 0);
    lv_obj_set_style_text_color(timer_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(timer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(timer_lbl, 10, 0);

    lv_obj_t* sp_bot = lv_obj_create(scroll_area);
    ui::clear_container(sp_bot);
    lv_obj_set_size(sp_bot, 0, 0);
    lv_obj_set_flex_grow(sp_bot, 1);

    // End call = ANCS Negative action (hang up). Danger-styled like Decline.
    lv_obj_t* end_btn = ui::make_btn(actions, "End call", ui::BtnStyle::Danger,
                                     nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(end_btn, on_end_call, LV_EVENT_CLICKED, ctx);

    // Hide (keep the call + timer going, just clear the overlay) is the top-right X.
    ui::add_close_x(card, on_dismiss, ctx);

    // Bind the label to the session and render the current elapsed time now, so
    // a reopened dialog shows the running duration immediately (not 00:00).
    g_session.label = timer_lbl;
    char buf[16];
    fmt_duration(buf, sizeof(buf), g_session.elapsed_s);
    lv_label_set_text(timer_lbl, buf);
}

} // namespace

namespace modal_incoming_call {

void show(uint32_t uid) {
    // Replace any overlay already up (latest call wins).
    if (g_call_scrim) lv_obj_delete(g_call_scrim);

    const app_state::AncsNotification& n = app_state::ancs_notification_by_uid(uid);

    auto* ctx = new CallCtx();
    ui::ModalLayout layout = ui::make_modal_layout(lv_screen_active(), 660, 380);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* card        = layout.card;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    ctx->scrim    = scrim;
    ctx->uid      = uid;
    g_call_scrim  = scrim;
    g_call_uid    = uid;
    g_call_active = false;  // ringing banner, not the in-call dialog
    lv_obj_add_event_cb(scrim, on_scrim_delete, LV_EVENT_DELETE, ctx);

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

    lv_obj_t* answer = ui::make_btn(actions, "Answer", ui::BtnStyle::Primary,
                                    nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(answer, on_answer, LV_EVENT_CLICKED, ctx);

    lv_obj_t* decline = ui::make_btn(actions, "Decline", ui::BtnStyle::Danger,
                                     nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(decline, on_decline, LV_EVENT_CLICKED, ctx);

    // Dismiss (hide the banner; the call keeps ringing) is the top-right X.
    ui::add_close_x(card, on_dismiss, ctx);
}

void notify_active(uint32_t uid) {
    // The call is active (answered). Keep the duration session running and
    // auto-present the dialog once; don't re-pop it on later ANCS events if the
    // user has since hidden it (they can reopen via the status-bar icon).
    session_start(uid);
    if (!g_session.auto_shown) {
        g_session.auto_shown = true;
        present_active_dialog(uid);
    }
}

void show_active(uint32_t uid) {
    // Explicit open (e.g. tapping the call's status-bar icon): ensure the
    // session is running and present the dialog with the current elapsed time.
    session_start(uid);
    g_session.auto_shown = true;
    present_active_dialog(uid);
}

void close_if_showing(uint32_t uid) {
    // ANCS Removed → the call ended: stop the duration timer and close any
    // overlay showing this UID. UID 0 is a valid ANCS UID.
    if (g_session.running && g_session.uid == uid) session_stop();
    if (g_call_scrim && g_call_uid == uid) lv_obj_delete(g_call_scrim);
}

void close_all() {
    // iPhone (ANCS) link dropped: the call is gone with it. Stop the duration
    // timer and tear down any open banner/dialog so nothing stale lingers.
    session_stop();
    if (g_call_scrim) lv_obj_delete(g_call_scrim);
}

} // namespace modal_incoming_call
