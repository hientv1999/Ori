#include "screens/modal_ancs_notification.h"

#include <cstring>
#include <lvgl.h>

#include "app_state.h"
#include "ble/ancs_client.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_status_bar.h"

// ANCS notification detail overlay.
//
// Notifications that share the same app AND title (e.g. several messages from
// one sender) are STACKED into a single overlay: the title + app name show
// once, with each message's body + timestamp listed newest-first, and a single
// "Read all" clears the whole group on the iPhone at once. A lone notification
// renders the same as before with a plain "Read".
//
//   title                         ← font_title (24px), shared, shown once
//   [N messages]                  ← font_meta, accent — only when stacked
//   per message: subtitle/body/time, oldest→newest (top→bottom), divider-split
//   app name                      ← font_h2 (28px), shared, shown once
//   [Read / Read all]   + top-right X

namespace {

struct ModalCtx {
    lv_obj_t* scrim;
    uint32_t  uids[app_state::MAX_ANCS_NOTIFICATIONS];  // every UID in the group
    size_t    count;
};

// The single currently-open detail overlay (only one can be open at a time —
// the scrim covers the status bar, so no second icon is tappable). Tracked so a
// remote ANCS Removed event can dismiss it via close_if_showing().
lv_obj_t* g_open_scrim = nullptr;
uint32_t  g_open_uid   = 0;   // reference (newest) UID of the open group

void on_read(lv_event_t* e) {
    auto* ctx = static_cast<ModalCtx*>(lv_event_get_user_data(e));
    // "Read" / "Read all": clear every stacked notification on the iPhone (ANCS
    // PerformNotificationAction · Negative) and remove each from Ori's queue +
    // detail + status bar. UID 0 is a valid ANCS UID, so dismiss unconditionally.
    for (size_t i = 0; i < ctx->count; ++i) {
        ancs_client::dismiss_notification(ctx->uids[i]);
    }
    lv_obj_delete(ctx->scrim);
    // ctx is freed by the scrim's LV_EVENT_DELETE handler below.
}

void on_close(lv_event_t* e) {
    auto* ctx = static_cast<ModalCtx*>(lv_event_get_user_data(e));
    lv_obj_delete(ctx->scrim);
}

// One body line (font_meta, secondary, wrapping).
lv_obj_t* add_text(lv_obj_t* parent, const char* text, const lv_font_t* font,
                   uint32_t color, int16_t pad_top, bool wrap) {
    lv_obj_t* lbl = lv_label_create(parent);
    if (wrap) {
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, lv_pct(100));
    }
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, theme::color(color), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(lbl, pad_top, 0);
    return lbl;
}

// Separator between stacked messages: a transparent 18 px row with a thin
// centred line, giving even spacing above and below.
void add_divider(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    ui::clear_container(row);
    lv_obj_set_size(row, lv_pct(100), 18);
    lv_obj_t* line = lv_obj_create(row);
    lv_obj_set_size(line, lv_pct(58), 2);
    lv_obj_center(line);
    lv_obj_set_style_bg_color(line, theme::color(theme::COLOR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 1, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
}

} // namespace

namespace modal_ancs_notification {

lv_obj_t* create(lv_obj_t* base_screen, uint32_t uid) {
    auto* ctx = new ModalCtx();

    // Gather all notifications sharing this one's app + title so they stack and
    // Read together. Falls back to just this UID when nothing else matches.
    ctx->count = app_state::ancs_collect_same_title(uid, ctx->uids,
                                                     app_state::MAX_ANCS_NOTIFICATIONS);
    if (ctx->count == 0) { ctx->uids[0] = uid; ctx->count = 1; }
    const bool stacked = ctx->count > 1;

    // The reference (newest) supplies the shared title + app name. Copy them now
    // — ancs_notification_by_uid() returns a shared view overwritten on each call
    // (the per-message loop below reuses it).
    char title_buf[193] = {};
    char app_buf[40]    = {};
    {
        const app_state::AncsNotification& ref =
            app_state::ancs_notification_by_uid(ctx->uids[0]);
        strncpy(title_buf, (ref.title && ref.title[0]) ? ref.title : "Notification",
                sizeof(title_buf) - 1);
        strncpy(app_buf, ref.display_name ? ref.display_name : "", sizeof(app_buf) - 1);
    }

    ui::ModalLayout layout = ui::make_modal_layout(base_screen, 660, 400);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* card        = layout.card;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    ctx->scrim   = scrim;
    g_open_scrim = scrim;
    g_open_uid   = ctx->uids[0];
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

    // ── Shared title (shown once) ──
    add_text(scroll_area, title_buf, theme::font_title(),
             theme::COLOR_TEXT_PRIMARY, 8, /*wrap=*/true);

    // ── "N messages" count — only when stacked ──
    if (stacked) {
        char cnt[24];
        lv_snprintf(cnt, sizeof(cnt), "%u messages", (unsigned)ctx->count);
        add_text(scroll_area, cnt, theme::font_meta(), theme::COLOR_ACCENT, 4, /*wrap=*/false);
    }

    // ── Each message: subtitle (if any) / body / timestamp ──
    // Render oldest → newest (top → bottom). ctx->uids is newest-first, so walk
    // it in reverse; `pos` is the on-screen row (0 = top = oldest).
    for (size_t pos = 0; pos < ctx->count; ++pos) {
        size_t i = ctx->count - 1 - pos;
        const app_state::AncsNotification& nv =
            app_state::ancs_notification_by_uid(ctx->uids[i]);

        if (stacked && pos > 0) add_divider(scroll_area);

        bool has_sub = nv.subtitle && nv.subtitle[0];
        if (has_sub) {
            int16_t pad = (pos > 0) ? 0 : 6;
            add_text(scroll_area, nv.subtitle, theme::font_meta(),
                     theme::COLOR_TEXT_SECONDARY, pad, /*wrap=*/true);
        }

        int16_t body_pad = has_sub ? 4 : (pos > 0 ? 0 : (stacked ? 6 : 16));
        add_text(scroll_area, nv.body, theme::font_meta(),
                 theme::COLOR_TEXT_SECONDARY, body_pad, /*wrap=*/true);

        if (nv.time_ago && nv.time_ago[0]) {
            add_text(scroll_area, nv.time_ago, theme::font_meta(),
                     theme::COLOR_TEXT_TERTIARY, 4, /*wrap=*/false);
        }
    }

    // ── Shared app name (shown once, bottom) ──
    add_text(scroll_area, app_buf, theme::font_h2(),
             theme::COLOR_TEXT_PRIMARY, 12, /*wrap=*/false);

    // Bottom spacer — mirrors top spacer to keep content vertically centred.
    lv_obj_t* sa_spacer_bot = lv_obj_create(scroll_area);
    ui::clear_container(sa_spacer_bot);
    lv_obj_set_size(sa_spacer_bot, 0, 0);
    lv_obj_set_flex_grow(sa_spacer_bot, 1);

    lv_obj_t* read_btn = ui::make_btn(actions, stacked ? "Read all" : "Read",
                                      ui::BtnStyle::Primary, nullptr, nullptr,
                                      12, 26, theme::font_meta());
    lv_obj_add_event_cb(read_btn, on_read, LV_EVENT_CLICKED, ctx);

    // Close affordance is the top-right X on the card.
    ui::add_close_x(card, on_close, ctx);

    return scrim;
}

void close_if_showing(uint32_t uid) {
    // UID 0 is a valid ANCS UID — match on the open scrim + reference UID.
    if (g_open_scrim && g_open_uid == uid) {
        lv_obj_delete(g_open_scrim);  // fires LV_EVENT_DELETE → clears globals + frees ctx
    }
}

} // namespace modal_ancs_notification
