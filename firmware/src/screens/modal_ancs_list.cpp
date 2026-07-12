#include "screens/modal_ancs_list.h"

#include <cstdlib>
#include <lvgl.h>

#include "app_state.h"
#include "assets/ancs_badge_icons.h"
#include "assets/ancs_icons.h"
#include "ble/ancs_client.h"
#include "screens/modal_ancs_notification.h"
#include "screens/modal_iphone_info.h"
#include "theme.h"
#include "ui_helpers.h"

// ANCS drill-down list — see modal_ancs_list.h for the full interaction spec.
//
// Layout (top to bottom, inside make_modal_layout's card):
//   title (bucket name, fixed above the scrollable body — same
//          non-scrolling-header technique as modal_ancs_notification's
//          header_row: a real flex child of `card` inserted before
//          scroll_area, not an IGNORE_LAYOUT overlay)
//   ── scrollable body (scroll_area) ──
//     one row per group: icon circle · title (1 line, ellipsis) ·
//     preview/body (1 line, ellipsis) — no timestamp (that's detail-only)
//   Back (tertiary) — replaces this modal with modal_iphone_info
//
// LIVE while open (modal_ancs_list::refresh_active(), see below) — unlike
// modal_iphone_info's badge counts (mostly a snapshot with one exception),
// this list rebuilds from ancs_client::list_bucket_groups() whenever the
// filter changes, a new notification arrives, or one leaves (from the
// iPhone's own ANCS events, an Orion-issued action, FIFO eviction, or
// another instance of this same swipe-to-delete) — see g_active_list's doc
// comment for why the queue-driven cases go through a deferred flag
// (ancs_client.cpp's g_ancs_list_refresh_pending) instead of an immediate
// call, and ancs_client::set_filter() for the filter case, which doesn't
// need deferring.

namespace {

constexpr lv_coord_t CARD_W = 660;
constexpr lv_coord_t CARD_H = 420;

// Icon circle matches ancs_icons' native asset size (convert_icons.py
// SIZE=(60,60)) and the status bar's own tile size (widget_status_bar.cpp
// ICON_SIZE) — 1:1, no scaling blur.
constexpr int16_t ROW_ICON_SIZE = 60;

// Ignore sub-pixel jitter before committing this drag to an axis, mirroring
// the prototype's 6px guard (Ori_UI_Prototype.js ancsSwipeMove) so a plain
// tap never gets misread as the start of a swipe.
constexpr int16_t SWIPE_JITTER_PX    = 6;
constexpr float    SWIPE_COMMIT_FRAC = 0.35f;  // fraction of row width that commits the delete
constexpr uint32_t SWIPE_ANIM_MS     = 180;

// Order matches ancs_client::ListBucket::MISSED/UNREAD/OTHER (0/1/2).
const char* const BUCKET_TITLE[3] = { "Calls", "Messages", "Notifications" };
const char* const BUCKET_EMPTY[3] = { "No calls", "No messages", "No notifications" };

// Shared per-modal state — freed on the scrim's LV_EVENT_DELETE.
struct ListCtx {
    lv_obj_t* scroll_area;
    lv_obj_t* base_screen;
    lv_obj_t* scrim;
    uint8_t   bucket;
    bool      connected;
};

// Per-row swipe/tap tracking — freed on the row's own LV_EVENT_DELETE.
struct RowState {
    ListCtx* list;
    uint32_t uid;       // group's representative (newest) uid
    int      start_x  = 0;
    int      start_y  = 0;
    int      width    = 0;   // row's own width, captured at LV_EVENT_PRESSED
    int      dx       = 0;
    bool     tracking = false;  // true between PRESSED and RELEASED/PRESS_LOST
    bool     dragging = false;  // true once this press engaged a left-swipe
};

void show_empty_state(ListCtx* list) {
    lv_obj_t* lbl = lv_label_create(list->scroll_area);
    lv_label_set_text(lbl, BUCKET_EMPTY[list->bucket]);
    lv_obj_set_style_text_font(lbl, theme::font_meta(), 0);
    lv_obj_set_style_text_color(lbl, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(lbl, 48, 0);
}

void row_translate_cb(void* obj, int32_t v) {
    lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), (int16_t)v, 0);
}
void row_opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), (lv_opa_t)v, 0);
}

// Resolves the row's full stacked-group uid set and clears every one of them
// (ANCS negative action where available, local drop otherwise — identical
// per-uid choice to modal_ancs_notification.cpp's on_read()), then deletes
// the row itself. If that empties the list, shows the empty-state label.
void commit_row_delete(lv_obj_t* row) {
    auto* rs = static_cast<RowState*>(lv_obj_get_user_data(row));
    if (!rs) { lv_obj_delete(row); return; }
    const uint32_t ref_uid = rs->uid;
    ListCtx* list = rs->list;

    uint32_t uids[app_state::MAX_ANCS_NOTIFICATIONS];
    size_t cnt = app_state::ancs_collect_same_title(ref_uid, uids,
                                                     app_state::MAX_ANCS_NOTIFICATIONS);
    if (cnt == 0) { uids[0] = ref_uid; cnt = 1; }
    for (size_t i = 0; i < cnt; ++i) {
        const app_state::AncsNotification& nv = app_state::ancs_notification_by_uid(uids[i]);
        if (nv.has_neg_action) ancs_client::dismiss_notification(uids[i]);
        else                   ancs_client::drop_notification(uids[i]);
    }

    lv_obj_delete(row);  // fires row's own LV_EVENT_DELETE -> frees rs

    if (list && lv_obj_get_child_count(list->scroll_area) == 0) {
        show_empty_state(list);
    }
}

void on_row_swipe_committed(lv_anim_t* a) {
    commit_row_delete(static_cast<lv_obj_t*>(a->var));
}

// Horizontal-left-only swipe-to-delete, adapted from screen_media_mode.cpp's
// on_art_gesture (PRESSED/PRESSING/RELEASED/PRESS_LOST + lv_indev_get_point).
// No LVGL scroll interference: scroll_area only scrolls LV_DIR_VER (see
// make_modal_layout), so a horizontal drag never gets captured as a list
// scroll — but LVGL still fires LV_EVENT_CLICKED at release regardless of
// how far the pointer moved (as long as no scroll engaged), so on_row_clicked
// below consults the same `dragging` flag to suppress opening the detail
// overlay after a swipe (mirrors the prototype's own suppressClick need).
void on_row_gesture(lv_event_t* e) {
    auto* rs = static_cast<RowState*>(lv_event_get_user_data(e));
    if (!rs) return;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        rs->start_x  = p.x;
        rs->start_y  = p.y;
        rs->width    = lv_obj_get_width(row);
        rs->dx       = 0;
        rs->tracking = true;
        rs->dragging = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (!rs->tracking) return;
        int dx = p.x - rs->start_x;
        int dy = p.y - rs->start_y;
        if (!rs->dragging) {
            if (abs(dx) < SWIPE_JITTER_PX && abs(dy) < SWIPE_JITTER_PX) return;
            // Only engage for a horizontal, leftward drag — a vertical-
            // dominant or rightward movement is left alone so the list's own
            // vertical scroll and plain taps keep working (gestures.md).
            if (abs(dy) > abs(dx) || dx > 0) return;
            rs->dragging = true;
        }
        int clamped = dx;
        if (clamped > 0) clamped = 0;
        if (rs->width > 0 && clamped < -rs->width) clamped = -rs->width;
        rs->dx = clamped;
        lv_obj_set_style_translate_x(row, (int16_t)clamped, 0);
        // Fade proportionally to drag distance — the fade itself is the
        // delete affordance, no revealed panel underneath (matches the
        // prototype's ancsSwipeMove).
        lv_opa_t opa = LV_OPA_COVER;
        if (rs->width > 0) {
            opa = (lv_opa_t)(LV_OPA_COVER - (LV_OPA_COVER * (-clamped)) / rs->width);
        }
        lv_obj_set_style_opa(row, opa, 0);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!rs->tracking) return;
        rs->tracking = false;
        if (!rs->dragging) return;  // plain tap — LV_EVENT_CLICKED handles it

        const bool committed = rs->width > 0 &&
            (-rs->dx) > (int)(rs->width * SWIPE_COMMIT_FRAC);

        lv_anim_t tr; lv_anim_init(&tr);
        lv_anim_set_var(&tr, row);
        lv_anim_set_exec_cb(&tr, row_translate_cb);
        lv_anim_set_values(&tr, rs->dx, committed ? -rs->width : 0);
        lv_anim_set_duration(&tr, SWIPE_ANIM_MS);
        lv_anim_set_path_cb(&tr, lv_anim_path_ease_out);
        lv_anim_start(&tr);

        lv_anim_t op; lv_anim_init(&op);
        lv_anim_set_var(&op, row);
        lv_anim_set_exec_cb(&op, row_opa_cb);
        lv_anim_set_values(&op, lv_obj_get_style_opa(row, LV_PART_MAIN),
                           committed ? LV_OPA_TRANSP : LV_OPA_COVER);
        lv_anim_set_duration(&op, SWIPE_ANIM_MS);
        lv_anim_set_path_cb(&op, lv_anim_path_ease_out);
        if (committed) lv_anim_set_completed_cb(&op, on_row_swipe_committed);
        lv_anim_start(&op);
        // Not resetting rs->dragging here: on_row_clicked (fired right after
        // this RELEASED, per LVGL's own event order) must still see it as
        // true — whether committed or snapped back, this press was a drag,
        // not a tap, so opening the detail overlay must be suppressed either
        // way (matches the prototype: suppressClick is set on ANY drag).
    }
}

void on_row_clicked(lv_event_t* e) {
    auto* rs = static_cast<RowState*>(lv_event_get_user_data(e));
    if (!rs) return;
    if (rs->dragging) { rs->dragging = false; return; }  // suppress: this click followed a swipe
    // Detail stacks on top of this list (same base_screen, not this modal's
    // own scrim) — see modal_ancs_list.h.
    modal_ancs_notification::open_for_uid(rs->list->base_screen, rs->uid);
}

// One row per notification GROUP — icon + title + latest preview, no
// timestamp (shown only once the detail overlay is opened).
lv_obj_t* make_row(lv_obj_t* parent, ListCtx* list, const ancs_client::ListGroup& g) {
    const app_state::AncsNotification& nv = app_state::ancs_notification_by_uid(g.uid);

    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, theme::color(theme::COLOR_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 16, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, theme::color(theme::COLOR_ELEV), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Icon circle — brand icon when known, else the category fallback,
    // tinted via the app's own brand colour as the circle's background
    // (widget_status_bar.cpp's make_ancs_tile pattern).
    lv_obj_t* icon_wrap = lv_obj_create(row);
    lv_obj_set_size(icon_wrap, ROW_ICON_SIZE, ROW_ICON_SIZE);
    lv_obj_set_style_bg_color(icon_wrap, theme::color(ancs_icons::color(g.icon_token)), 0);
    lv_obj_set_style_bg_opa(icon_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_wrap, 0, 0);
    lv_obj_set_style_pad_all(icon_wrap, 0, 0);
    lv_obj_set_style_radius(icon_wrap, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(icon_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(icon_wrap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(icon_wrap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // count/silent badge overhangs the corner

    const lv_image_dsc_t* img = ancs_icons::image(g.icon_token);
    if (!img) img = ancs_icons::category_image(app_state::ancs_category(g.uid));
    if (img) {
        lv_obj_t* icon_img = lv_image_create(icon_wrap);
        lv_image_set_src(icon_img, img);
        lv_obj_center(icon_img);
        lv_obj_clear_flag(icon_img, LV_OBJ_FLAG_CLICKABLE);
    }

    // Count badge (top-right) / silent badge (top-left) — same 26px circle
    // style, different corners, and mutually exclusive in what they show: a
    // stacked group's count always wins over its representative's silent
    // flag (a silent badge on a 2+ stack would be misleading — silence is a
    // per-notification ANCS flag, not a property of the whole group).
    if (g.count > 1) {
        char buf[5];
        if (g.count > 9) { buf[0] = '9'; buf[1] = '+'; buf[2] = '\0'; }
        else             lv_snprintf(buf, sizeof(buf), "%u", (unsigned)g.count);

        lv_obj_t* badge = lv_obj_create(icon_wrap);
        lv_obj_set_size(badge, 26, 26);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(badge, theme::color(theme::COLOR_DANGER), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        // Border matches the card bg, faking a cutout — same trick
        // modal_iphone_info's make_stat_unit badge uses.
        lv_obj_set_style_border_width(badge, 3, 0);
        lv_obj_set_style_border_color(badge, theme::color(theme::COLOR_CARD), 0);
        lv_obj_set_style_border_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(badge, 0, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 0, 0);

        lv_obj_t* lbl = lv_label_create(badge);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, theme::font_body(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
    } else if (nv.silent) {
        // Small icon-only badge (bell-off glyph) — same 26px size and
        // card-coloured cutout border as the count badge above, just with a
        // small icon instead of a number (design feedback: "a small icon in
        // the top right corner... not a big badge on top of the icon" —
        // previously a wider "Silent" text pill overlapping much more of
        // the icon). Pinned to the TOP-LEFT corner rather than top-right —
        // matches modal_ancs_notification.cpp's detail-overlay silent badge
        // placement (pc-app.md: "silent badge top-left, close always
        // top-right"), so the glyph means the same thing in the same corner
        // everywhere it appears. Never actually competes with the count
        // badge above for space since the two are mutually exclusive.
        lv_obj_t* badge = lv_obj_create(icon_wrap);
        lv_obj_set_size(badge, 26, 26);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        // Border matches the card bg, faking a cutout — same trick the count
        // badge above uses.
        lv_obj_set_style_border_width(badge, 3, 0);
        lv_obj_set_style_border_color(badge, theme::color(theme::COLOR_CARD), 0);
        lv_obj_set_style_border_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(badge, 0, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t* icon = lv_image_create(badge);
        lv_image_set_src(icon, ancs_badge_icons::silent());
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(icon);
        lv_obj_set_style_image_recolor(icon, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    }

    // Title + preview — single line each, ellipsis on overflow (same
    // LV_LABEL_LONG_DOT + explicit 1-line-height technique
    // screen_meeting_list.cpp's rows use).
    lv_obj_t* text_col = lv_obj_create(row);
    ui::clear_container(text_col);
    lv_obj_set_height(text_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text_col, 1);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const char* title_text = (nv.title && nv.title[0]) ? nv.title : "Notification";
    lv_obj_t* title = lv_label_create(text_col);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_height(title, theme::font_title()->line_height);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_font(title, theme::font_title(), 0);
    lv_obj_set_style_text_color(title, theme::color(theme::COLOR_TEXT_PRIMARY), 0);

    lv_obj_t* preview = lv_label_create(text_col);
    lv_label_set_long_mode(preview, LV_LABEL_LONG_DOT);
    lv_obj_set_width(preview, lv_pct(100));
    lv_obj_set_height(preview, theme::font_meta()->line_height);
    lv_label_set_text(preview, nv.body ? nv.body : "");
    lv_obj_set_style_text_font(preview, theme::font_meta(), 0);
    lv_obj_set_style_text_color(preview, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_pad_top(preview, 4, 0);

    // Per-row swipe/tap state.
    auto* rs = new RowState();
    rs->list = list;
    rs->uid  = g.uid;
    lv_obj_set_user_data(row, rs);
    lv_obj_add_event_cb(row, on_row_gesture, LV_EVENT_PRESSED,    rs);
    lv_obj_add_event_cb(row, on_row_gesture, LV_EVENT_PRESSING,   rs);
    lv_obj_add_event_cb(row, on_row_gesture, LV_EVENT_RELEASED,   rs);
    lv_obj_add_event_cb(row, on_row_gesture, LV_EVENT_PRESS_LOST, rs);
    lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED,    rs);
    lv_obj_add_event_cb(row, [](lv_event_t* e) {
        delete static_cast<RowState*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, rs);

    return row;
}

// Weak reference to the currently-open list (nullptr if none is open) — same
// weak-ref-refresh technique as widget_status_bar::g_active_bar /
// modal_iphone_info::g_active_stats_row. Lets ancs_client push a live
// rebuild when the underlying queue or filter changes while this list is on
// screen (see modal_ancs_list.h's interaction spec). Cleared on the scrim's
// own LV_EVENT_DELETE. `ListCtx` already carries everything a rebuild needs
// (scroll_area/bucket/...), so one pointer suffices — no need for
// modal_iphone_info's several separate globals.
ListCtx* g_active_list = nullptr;

// Rebuildable body of scroll_area — shared by create() (first build) and
// modal_ancs_list::refresh_active() (rebuild in place after the queue or
// filter changes). Re-reads ancs_client::list_bucket_groups() fresh each
// call, same "always show what's actually live" approach as
// modal_iphone_info's populate_stats_row().
void populate_list(ListCtx* list) {
    lv_obj_clean(list->scroll_area);
    ancs_client::ListGroup groups[app_state::MAX_ANCS_NOTIFICATIONS];
    size_t n = ancs_client::list_bucket_groups(list->bucket, groups,
                                                app_state::MAX_ANCS_NOTIFICATIONS);
    if (n == 0) {
        show_empty_state(list);
    } else {
        for (size_t i = 0; i < n; ++i) make_row(list->scroll_area, list, groups[i]);
    }
}

} // namespace

namespace modal_ancs_list {

lv_obj_t* create(lv_obj_t* base_screen, uint8_t bucket, bool connected) {
    ui::ModalLayout layout = ui::make_modal_layout(base_screen, CARD_W, CARD_H);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* card        = layout.card;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    // Title — fixed above the scrollable body. A real flex child of `card`
    // inserted before scroll_area (not an IGNORE_LAYOUT overlay), same
    // non-scrolling-header technique as modal_ancs_notification's
    // header_row: scroll_area is pushed down/shrunk by its height
    // automatically, so scrolled rows can never pass underneath it.
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, BUCKET_TITLE[bucket]);
    lv_obj_set_style_text_font(title, theme::font_h2(), 0);
    lv_obj_set_style_text_color(title, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_move_to_index(title, 0);

    lv_obj_set_style_pad_top(scroll_area, 10, 0);

    auto* list = new ListCtx();
    list->scroll_area = scroll_area;
    list->base_screen = base_screen;
    list->scrim       = scrim;
    list->bucket      = bucket;
    list->connected   = connected;

    populate_list(list);

    g_active_list = list;
    lv_obj_add_event_cb(scrim, [](lv_event_t* e) {
        g_active_list = nullptr;
        delete static_cast<ListCtx*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, list);

    // Back — replaces this modal with modal_iphone_info (not a stack: the
    // list is not something iPhone Info needs to keep alive underneath it).
    lv_obj_t* back = ui::make_btn(actions, "Back", ui::BtnStyle::Tertiary,
                                  nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(back, [](lv_event_t* e) {
        auto* l = static_cast<ListCtx*>(lv_event_get_user_data(e));
        lv_obj_t* base = l->base_screen;
        bool     conn  = l->connected;
        lv_obj_delete(l->scrim);  // cascades: frees every row's RowState, then `l` itself
        modal_iphone_info::create(base, conn);
    }, LV_EVENT_CLICKED, list);

    return scrim;
}

void refresh_active() {
    if (!g_active_list) return;  // no instance currently open
    populate_list(g_active_list);
}

} // namespace modal_ancs_list
