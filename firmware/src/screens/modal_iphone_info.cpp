#include "screens/modal_iphone_info.h"

#include <Arduino.h>
#include <lvgl.h>
#include <cstdio>

#include "assets/iphone_info_icons.h"
#include "ble/ancs_client.h"
#include "screens/modal_ancs_list.h"
#include "screens/modal_unpair_phone.h"
#include "theme.h"
#include "ui_helpers.h"

// iPhone Info/Stats overlay. Tapping the status-bar phone icon while an
// iPhone is bonded now opens this read-only snapshot instead of jumping
// straight to the Unpair confirm — mirrors the Orion PC app's own iPhone
// Info modal and the approved Ori_UI_Prototype.html design (iOS-style count
// badge overlapping the corner of each icon — see that file's #m-iphone-info
// / .ip-stats / .ip-badge for the reference layout, ported 1:1 since both
// run on the same 800x480 canvas).
//
// Layout (top to bottom, inside make_modal_layout's scroll_area):
//   name (phone_name())
//   status dot + Connected/Disconnected  ···  signal bars
//   [call icon+badge] [message icon+badge] [bell icon+badge] — no per-icon
//     tile background/border (removed per design feedback: "Can you just
//     put 3 icons on same row without each of their block?")
//   Cancel (tertiary) · Unpair (danger)
//
// Counts/signal come from ancs_client::phone_stats() — all zero while
// disconnected, same "don't show what can't be verified" policy as
// presence/weather (ble-protocol.md §6.4). A zero count dims its icon
// (recolor to tertiary) and hides its badge entirely rather than showing a
// "0" — the icons stay visible/laid out either way, just look disabled.
// LIVE while open (2026-07-11 — previously "mostly a snapshot," which has
// been fully revoked; every value on this modal now tracks the live state
// it's read from):
//   - Stat badges refresh on an ANCS filter change (modal_iphone_info::
//     refresh_active(), called from ancs_client::set_filter()) and on any
//     queue change while connected (same trigger as modal_ancs_list, see
//     its own doc comment) — a stale count that contradicts the filter the
//     user just picked, or omits a notification that just arrived, would
//     look like a bug.
//   - Connection dot, status label, phone name, and stat-badge clickability
//     refresh on every iPhone connect/disconnect (modal_iphone_info::
//     set_connected(), called from state_machine::set_phone_connected() —
//     the same single choke-point widget_status_bar's own phone icon
//     already trusts as authoritative). The title always reflects whichever
//     iPhone is CURRENTLY connected — re-read fresh from
//     ancs_client::phone_name() on every change, not cached from open.
//     (Out of scope: the never-bonded case doesn't reach this modal at all —
//     tapping the status-bar phone icon routes to the re-pair screen
//     instead, per gestures.md.)
//   - Signal bars refresh on every RSSI sample (modal_iphone_info::
//     set_signal_bars(), called from ancs_client::poll()'s periodic RSSI
//     poll) — previously only pushed to Orion, never to this modal, so the
//     bars could sit stale for the RSSI_POLL_INTERVAL_MS lifetime of the
//     modal being open.
//
// Tap a stat tile (while connected and its count is non-zero) to drill into
// that bucket's live notifications on-device — modal_ancs_list, opened in
// place of this modal (see modal_ancs_list.h). Orion's own mirror of this
// drill-down is a separate implementation on the PC side (pc-app.md); this
// is Ori's own on-device path.

namespace {

void on_unpair(lv_event_t* e) {
    lv_obj_t* scrim = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    lv_obj_t* base  = lv_obj_get_parent(scrim);  // scrim's parent is base_screen (make_scrim)
    lv_obj_delete(scrim);
    modal_unpair_phone::create(base);
}

// Three earlier attempts at these icons via hand-drawn LVGL primitives
// (rects/arcs/lines composited with lv_obj_align/align_to, then with
// absolute lv_obj_set_pos coordinates) all rendered incorrectly with no way
// to see the actual output and iterate. Replaced with real compiled-in
// raster icons — same pipeline as the media-mode shortcut buttons
// (firmware/img/shortcut_icons/convert_shortcut_icons.py): a PNG run
// through LVGLImage.py into an ARGB8888 lv_image_dsc_t. See
// firmware/img/iphone_info_icons/convert_iphone_info_icons.py and
// assets/iphone_info_icons.h. Only the source PNG's alpha channel matters
// (the glyph shape); lv_obj_set_style_image_recolor at LV_OPA_COVER tints it
// to the actual/zero-state colour at render time — no per-state asset needed.
//
// The compiled assets are the user's own icon art (bell.png / missed_call.png /
// message.png dropped into iphone_info_icons/, replacing the rejected
// generated placeholders) — outline-style glyphs matching Orion's own
// iPhone Info modal icons (PC_app/orion/src/index.html's ipInfoMissedIco/
// ipInfoUnreadIco/ipInfoNotifIco `<svg>` glyphs) so both sides of the sync
// show visually consistent iconography. Baked at the box's native 70x70
// (convert script SIZE) — drawn 1:1 with no LVGL scale transform. An
// earlier 28px-source + 2.5x-upscale attempt rendered visibly soft.
constexpr int32_t ICON_BOX      = 70;

// The count badge (below) deliberately overhangs wrap's own 70x70 box —
// aligned TOP_RIGHT at (+14,-11), matching Ori_UI_Prototype.html's .ip-badge
// (top:-11px/right:-14px). LV_OBJ_FLAG_OVERFLOW_VISIBLE on `wrap` alone does
// NOT stop that overhang being clipped: per LVGL's lv_refr.c, the clip area
// a parent hands to its children is the parent's own box widened by
// lv_obj_get_ext_draw_size(parent) — not simply "unclipped" — and a plain
// lv_obj with no shadow/outline styling reports ext_draw_size 0 by default,
// so the badge's protruding top-right sliver was silently cut off at
// wrap's edge. BADGE_OVERHANG is registered via a LV_EVENT_REFR_EXT_DRAW_SIZE
// handler below (the same mechanism LVGL's own widgets, e.g. lv_arc, use to
// grow their clip margin) — sized to cover the larger of the two overhangs
// (14 right, 11 top) plus a little slack.
constexpr int32_t BADGE_OVERHANG = 16;

// Per-tile context for the tap-to-drill-down handler below — heap-allocated,
// freed on the tile's own LV_EVENT_DELETE (so it's cleaned up regardless of
// which path closes this modal: Cancel, Unpair, or the tile tap itself).
struct StatClickCtx {
    lv_obj_t* scrim;
    lv_obj_t* base_screen;
    uint8_t   bucket;
    bool      connected;
};

// One icon + overlapping count badge (Ori_UI_Prototype.html .ip-icon-wrap /
// .ip-badge). The badge's box-shadow ring (matches the card bg, faking a
// cutout) becomes an LVGL border in the same colour, since LVGL has no
// box-shadow-as-ring primitive.
//
// Tappable whenever `connected` — even at zero count, opening
// modal_ancs_list to show its empty state ("No missed calls" etc.) rather
// than being inert. A dimmed icon communicates "nothing here right now,"
// not "you can't check." Only truly gated on `connected`: while
// disconnected there's no live data to open a list onto at all (same "don't
// show what can't be verified" policy as everywhere else this modal reads
// from ancs_client::phone_stats()).
lv_obj_t* make_stat_unit(lv_obj_t* parent, const lv_image_dsc_t* icon_dsc,
                          uint8_t count, lv_obj_t* scrim, lv_obj_t* base_screen,
                          bool connected, uint8_t bucket) {
    lv_obj_t* wrap = lv_obj_create(parent);
    ui::clear_container(wrap);
    lv_obj_set_size(wrap, ICON_BOX, ICON_BOX);
    // OVERFLOW_VISIBLE + a widened ext_draw_size (BADGE_OVERHANG's doc
    // comment above) is what actually keeps the badge's overhang from being
    // clipped — the flag alone is not sufficient.
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(wrap, [](lv_event_t* e) {
        int32_t* s = static_cast<int32_t*>(lv_event_get_param(e));
        *s = LV_MAX(*s, BADGE_OVERHANG);
    }, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
    // Not tied to any style change, so it never fires on its own — force one
    // computation now.
    lv_obj_refresh_ext_draw_size(wrap);

    const bool clickable = connected;
    if (clickable) {
        lv_obj_add_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(wrap, LV_OPA_60, LV_STATE_PRESSED);

        auto* ctx = new StatClickCtx{scrim, base_screen, bucket, connected};
        lv_obj_add_event_cb(wrap, [](lv_event_t* e) {
            auto* c = static_cast<StatClickCtx*>(lv_event_get_user_data(e));
            lv_obj_t* base = c->base_screen;
            uint8_t   b    = c->bucket;
            bool      conn = c->connected;
            lv_obj_delete(c->scrim);  // cascades: frees `c` via this tile's own DELETE below
            modal_ancs_list::create(base, b, conn);
        }, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(wrap, [](lv_event_t* e) {
            delete static_cast<StatClickCtx*>(lv_event_get_user_data(e));
        }, LV_EVENT_DELETE, ctx);
    }

    lv_obj_t* icon = lv_image_create(wrap);
    lv_image_set_src(icon, icon_dsc);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(icon);
    uint32_t icon_color = (count == 0) ? theme::COLOR_TEXT_TERTIARY : theme::COLOR_TEXT_SECONDARY;
    lv_obj_set_style_image_recolor(icon, theme::color(icon_color), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);

    lv_obj_t* badge = lv_obj_create(wrap);
    lv_obj_set_height(badge, 36);
    lv_obj_set_width(badge, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(badge, 36, 0);
    lv_obj_set_style_pad_left(badge, 8, 0);
    lv_obj_set_style_pad_right(badge, 8, 0);
    lv_obj_set_style_pad_top(badge, 0, 0);
    lv_obj_set_style_pad_bottom(badge, 0, 0);
    lv_obj_set_style_bg_color(badge, theme::color(theme::COLOR_DANGER), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(badge, theme::color(theme::COLOR_CARD), 0);
    lv_obj_set_style_border_width(badge, 4, 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    // IGNORE_LAYOUT + align pins the badge to wrap's top-right corner
    // regardless of wrap having no flex layout of its own (same technique as
    // ui::add_close_x's corner-pinned X button).
    lv_obj_add_flag(badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 14, -11);

    char buf[5];
    if (count > 99) snprintf(buf, sizeof(buf), "99+");
    else snprintf(buf, sizeof(buf), "%u", (unsigned)count);
    lv_obj_t* n = lv_label_create(badge);
    lv_label_set_text(n, buf);
    lv_obj_set_style_text_font(n, theme::font_body(), 0);
    lv_obj_set_style_text_color(n, theme::color(0xFFFFFF), 0);
    lv_obj_center(n);

    if (count == 0) lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);

    return wrap;
}

// Weak reference to the currently-open modal's live-updatable parts
// (nullptr when no instance is open) — same weak-ref-refresh technique as
// widget_status_bar::g_active_bar/refresh_active(). Lets ancs_client push
// live updates from wherever the underlying BLE state actually changes
// (set_filter(), state_machine::set_phone_connected(), the RSSI poll) —
// see the module header comment above. Cleared on the scrim's own
// LV_EVENT_DELETE.
struct ActiveInfo {
    lv_obj_t* scrim;
    lv_obj_t* base_screen;
    lv_obj_t* stats_row;
    lv_obj_t* name_lbl;
    lv_obj_t* dot;
    lv_obj_t* status_lbl;
    lv_obj_t* sig_bars[4];
    bool      connected;  // cached — needed by populate_stats_row() when
                           // refresh_active() rebuilds badges for a reason
                           // OTHER than a connection change (filter/queue)
};
ActiveInfo* g_active = nullptr;

// Rebuildable body of stats_row — shared by create() (first build) and
// modal_iphone_info::refresh_active() (rebuild in place after a filter
// change). Re-reads ancs_client::phone_stats() fresh each call.
void populate_stats_row(lv_obj_t* stats_row, lv_obj_t* scrim, lv_obj_t* base_screen,
                         bool connected) {
    lv_obj_clean(stats_row);
    const ancs_client::PhoneStats stats = ancs_client::phone_stats();
    make_stat_unit(stats_row, iphone_info_icons::missed_call(), stats.missed,
                   scrim, base_screen, connected, ancs_client::ListBucket::MISSED);
    make_stat_unit(stats_row, iphone_info_icons::message(), stats.unread,
                   scrim, base_screen, connected, ancs_client::ListBucket::UNREAD);
    make_stat_unit(stats_row, iphone_info_icons::bell(), stats.total,
                   scrim, base_screen, connected, ancs_client::ListBucket::OTHER);
}

} // namespace

namespace modal_iphone_info {

lv_obj_t* create(lv_obj_t* base_screen, bool connected) {
    // card_h=338 matches the approved (badge-redesign) prototype's own
    // .ip-card rendered height (re-measured via Playwright against
    // Ori_UI_Prototype.html at its native 560px card width, after the tile
    // blocks were removed and icons/badges replaced the old icon+number
    // layout — supersedes the earlier 377 figure from the pre-redesign tiles).
    ui::ModalLayout layout = ui::make_modal_layout(base_screen, 560, 338);
    lv_obj_t* scrim       = layout.scrim;
    lv_obj_t* scroll_area = layout.scroll_area;
    lv_obj_t* actions     = layout.actions;

    // make_modal_layout defaults scroll_area to flex_grow(1), filling all of
    // card_h left over after `actions` — if this modal's actual LVGL-
    // rendered content (font metrics here don't match the browser prototype
    // 1:1) comes out shorter than that allocation, the difference shows up
    // as dead space directly above the button row, since content aligns to
    // the top. Size scroll_area to its own content instead: the card's own
    // CENTER alignment (make_modal_layout) then absorbs any leftover height
    // by splitting it outside the content+button group — above the name and
    // below the buttons — instead of wedging it between the stat icons and
    // Cancel/Unpair.
    lv_obj_set_flex_grow(scroll_area, 0);
    lv_obj_set_height(scroll_area, LV_SIZE_CONTENT);

    const ancs_client::PhoneStats stats = ancs_client::phone_stats();
    const char* pname = ancs_client::phone_name();

    auto* info = new ActiveInfo();
    info->scrim       = scrim;
    info->base_screen = base_screen;
    info->connected   = connected;

    lv_obj_t* name = lv_label_create(scroll_area);
    lv_label_set_text(name, (pname && pname[0]) ? pname : "iPhone");
    lv_obj_set_style_text_color(name, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(name, theme::font_h2(), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    info->name_lbl = name;

    // Status row: dot + Connected/Disconnected on the left, signal bars on
    // the right — same combined row as the approved prototype design.
    lv_obj_t* status_row = lv_obj_create(scroll_area);
    ui::clear_container(status_row);
    lv_obj_set_width(status_row, lv_pct(100));
    lv_obj_set_height(status_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(status_row, 8, 0);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* status_left = lv_obj_create(status_row);
    ui::clear_container(status_left);
    lv_obj_set_size(status_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_left, 8, 0);

    lv_obj_t* dot = lv_obj_create(status_left);
    ui::clear_container(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, theme::color(
        connected ? theme::COLOR_PRESENCE_AVAILABLE : theme::COLOR_PRESENCE_OFFLINE), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t* status_lbl = lv_label_create(status_left);
    lv_label_set_text(status_lbl, connected ? "Connected" : "Disconnected");
    lv_obj_set_style_text_color(status_lbl, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(status_lbl, theme::font_meta(), 0);
    info->dot        = dot;
    info->status_lbl = status_lbl;

    // 8x33 bars, 5px gap — 1.5x the original size per feedback ("I also want
    // 1.5x [the RSSI bar's] size"), matching .sig-bars in the approved design.
    lv_obj_t* sig = lv_obj_create(status_row);
    ui::clear_container(sig);
    lv_obj_set_size(sig, LV_SIZE_CONTENT, 33);
    lv_obj_set_flex_flow(sig, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sig, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(sig, 5, 0);
    static const uint8_t bar_h[4] = {12, 18, 25, 33};  // 35/55/75/100% of 33px
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* bar = lv_obj_create(sig);
        ui::clear_container(bar);
        lv_obj_set_size(bar, 8, bar_h[i]);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_color(bar, theme::color(
            i < stats.signal_bars ? theme::COLOR_PRESENCE_AVAILABLE : theme::COLOR_DIVIDER_STRONG), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        info->sig_bars[i] = bar;
    }

    // Stat row: call / message / bell icons, each with an overlapping count
    // badge — no per-icon tile block (removed per design feedback), centred
    // as a group with a wide gap between icons (.ip-stats in the approved
    // design: justify-content:center, gap:56px).
    lv_obj_t* stats_row = lv_obj_create(scroll_area);
    ui::clear_container(stats_row);
    lv_obj_set_width(stats_row, lv_pct(100));
    lv_obj_set_height(stats_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(stats_row, 30, 0);
    lv_obj_add_flag(stats_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // badges overhang each icon's box
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(stats_row, 56, 0);

    populate_stats_row(stats_row, scrim, base_screen, connected);
    info->stats_row = stats_row;

    // Weak ref for the live-update entry points (module header comment) —
    // cleared when this modal closes, however it closes (Cancel/Unpair/tile
    // tap all end with lv_obj_delete(scrim)).
    g_active = info;
    lv_obj_add_event_cb(scrim, [](lv_event_t* e) {
        g_active = nullptr;
        delete static_cast<ActiveInfo*>(lv_event_get_user_data(e));
    }, LV_EVENT_DELETE, info);

    // actions' shared pad_top (make_modal_layout) is 8px; tightened to ~1/3
    // less here per feedback that the gap above the button row still read as
    // too large even after the scroll_area content-sizing fix above.
    lv_obj_set_style_pad_top(actions, 5, 0);

    // Cancel on the left, Unpair (danger) on the right.
    lv_obj_t* cancel = ui::make_btn(actions, "Cancel", ui::BtnStyle::Tertiary,
                                    nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(cancel, ui::close_scrim_cb, LV_EVENT_CLICKED, scrim);

    lv_obj_t* unpair = ui::make_btn(actions, "Unpair", ui::BtnStyle::Danger,
                                    nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(unpair, on_unpair, LV_EVENT_CLICKED, scrim);

    return scrim;
}

void refresh_active() {
    if (!g_active) return;  // no instance currently open
    populate_stats_row(g_active->stats_row, g_active->scrim, g_active->base_screen,
                        g_active->connected);
}

void set_connected(bool connected) {
    if (!g_active) return;  // no instance currently open
    g_active->connected = connected;

    lv_obj_set_style_bg_color(g_active->dot, theme::color(
        connected ? theme::COLOR_PRESENCE_AVAILABLE : theme::COLOR_PRESENCE_OFFLINE), 0);
    lv_label_set_text(g_active->status_lbl, connected ? "Connected" : "Disconnected");

    // Title always reflects whichever iPhone is CURRENTLY connected — re-read
    // fresh rather than trusting whatever was cached at open. On disconnect
    // ancs_client clears its cached name to "", so this correctly falls back
    // to the generic "iPhone" label until (if) it reconnects.
    const char* pname = ancs_client::phone_name();
    lv_label_set_text(g_active->name_lbl, (pname && pname[0]) ? pname : "iPhone");

    // Rebuild the stat badges too — their clickability is baked in at
    // construction (make_stat_unit's CLICKABLE flag + event registration),
    // not just a style, so a connect/disconnect needs the same full rebuild
    // populate_stats_row() already does for filter/queue changes, not just a
    // re-colour.
    populate_stats_row(g_active->stats_row, g_active->scrim, g_active->base_screen, connected);
}

void set_signal_bars(uint8_t bars) {
    if (!g_active) return;  // no instance currently open
    for (int i = 0; i < 4; ++i) {
        lv_obj_set_style_bg_color(g_active->sig_bars[i], theme::color(
            i < bars ? theme::COLOR_PRESENCE_AVAILABLE : theme::COLOR_DIVIDER_STRONG), 0);
    }
}

} // namespace modal_iphone_info
