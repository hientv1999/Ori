#include "widgets/widget_status_bar.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <time.h>

#include "assets/ancs_badge_icons.h"
#include "assets/ancs_icons.h"
#include "app_state.h"
#include "screens/modal_ancs_notification.h"
#include "screens/modal_incoming_call.h"
#include "screens/modal_iphone_info.h"
#include "screens/modal_unpair_phone.h"
#include "state_machine.h"
#include "screens/screen_setup.h" // for screen_setup::create
#include "theme.h"
#include "time_format.h"

// Status bar: 800 x 84 px, full panel width, anchored top.
//
// Visual reference: prototype `.status-bar` block.
//   - 22 px horizontal padding
//   - Time (30 px), separator dot, date (22 px) on the left
//   - ANCS icons (48 x 48, gap 14 px) on the right
//   - Phone icon (64 x 64) always visible — always neutral (secondary text)
//     colour; a diagonal slash is drawn across the glyph when disconnected.
//     Colour alone no longer encodes state (committed design pivot — the
//     slash is legible regardless of red/grey colour perception); tap →
//     unpair / re-pair
//   - 3 px bottom border in COLOR_DIVIDER
//
// ANCS icons are colored placeholders — the prototype's brand-tile look.
// Real raster/vector assets land in M8 (font_icons or LV_USE_PNG).

namespace {

constexpr int16_t PAD_X         = 12;
constexpr int16_t ICON_SIZE     = 60;
constexpr int16_t ICON_GAP      = 14;
constexpr int16_t PHONE_SIZE    = 64;
constexpr int16_t DATETIME_GAP  = 12;

// Format current local time into the display strings. Fills time_buf (the
// "H:MM" portion, always at font_time() size), ampm_buf (the "AM"/"PM"
// suffix — emptied in 24-hour format, since it renders as its own smaller
// subtext label rather than being appended to time_buf), and date_buf.
// Returns whether the clock is actually set. Before the first time sync
// (e.g. after a cold power cycle) there is no valid time, so every string is
// emptied and the caller hides the whole clock — no "--:--" placeholder, no
// fabricated ~1970 value.
static bool format_real_time(char* time_buf, size_t time_sz,
                              char* ampm_buf, size_t ampm_sz,
                              char* date_buf, size_t date_sz) {
    if (!app_state::clock_is_set()) {
        if (time_sz) time_buf[0] = '\0';
        if (ampm_sz) ampm_buf[0] = '\0';
        if (date_sz) date_buf[0] = '\0';
        return false;
    }
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    time_format::hhmm_split(time_buf, time_sz, ampm_buf, ampm_sz, tm.tm_hour, tm.tm_min);
    char day[4], mon[4];
    strftime(day, sizeof(day), "%a", &tm);
    strftime(mon, sizeof(mon), "%b", &tm);
    snprintf(date_buf, date_sz, "%s, %s %d", day, mon, tm.tm_mday);
    return true;
}

struct StatusBarState {
    lv_obj_t*   datetime_row;
    lv_obj_t*   time_row;    // wraps time_label + ampm_label with a tight gap
    lv_obj_t*   time_label;
    lv_obj_t*   ampm_label;  // "AM"/"PM" subtext — hidden in 24-hour format
    lv_obj_t*   sep_label;
    lv_obj_t*   date_label;
    // Last-rendered clock strings — update_clock_labels() runs every second on
    // almost every screen, but the displayed values only actually change once
    // a minute (time/ampm) or once a day (date). Caching lets it skip the
    // lv_label_set_text() + hidden-flag churn on the ~59/60 ticks where
    // nothing visibly changes.
    char        last_time_buf[8];
    char        last_ampm_buf[4];
    char        last_date_buf[20];
    bool        last_clock_set;
    bool        clock_labels_initialized; // false until the first tick renders
    lv_timer_t* clock_timer;  // 1 s self-update timer
    lv_timer_t* time_long_press_timer; // custom-duration press-and-hold timer (datetime_row only)
    bool        time_long_press_fired; // true once the timer above has fired, until the next press
    lv_obj_t* ancs_row;
    lv_obj_t* phone_icon;
    lv_obj_t* mode_toggle;       // 60x60 button at the right edge
    bool      show_datetime;
    bool      pc_connected;      // controls mode_toggle visibility
    bool      phone_bonded;      // true if a phone BLE bond exists
    bool      phone_connected;   // true if the iPhone BLE link is up
    int8_t    phone_glyph_state; // -1 = not drawn yet; 0/1 = last drawn state
    widget_status_bar::Mode mode;
    widget_status_bar::ModeToggleCb mode_cb;
    widget_status_bar::TimeTapCb    time_tap_cb;
    widget_status_bar::TimeLongPressCb time_long_press_cb;
};

// Update just the time/date labels — what the 1-second clock timer calls. The
// ANCS row is NOT rebuilt here (only on config change via refresh_active), so a
// new-icon entrance animation isn't restarted every second.
static void update_clock_labels(lv_obj_t* bar) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    char time_buf[8], ampm_buf[4], date_buf[20];
    bool clock_set = format_real_time(time_buf, sizeof(time_buf), ampm_buf, sizeof(ampm_buf),
                                       date_buf, sizeof(date_buf));

    // Skip the redraw entirely when nothing displayed actually changed since
    // last tick (time/ampm change once a minute, date once a day — this runs
    // every second on almost every screen the whole time Ori is powered on).
    bool unchanged = s->clock_labels_initialized &&
                      clock_set == s->last_clock_set &&
                      strcmp(time_buf, s->last_time_buf) == 0 &&
                      strcmp(ampm_buf, s->last_ampm_buf) == 0 &&
                      strcmp(date_buf, s->last_date_buf) == 0;
    if (unchanged) return;
    s->clock_labels_initialized = true;
    s->last_clock_set = clock_set;
    strncpy(s->last_time_buf, time_buf, sizeof(s->last_time_buf) - 1);
    s->last_time_buf[sizeof(s->last_time_buf) - 1] = '\0';
    strncpy(s->last_ampm_buf, ampm_buf, sizeof(s->last_ampm_buf) - 1);
    s->last_ampm_buf[sizeof(s->last_ampm_buf) - 1] = '\0';
    strncpy(s->last_date_buf, date_buf, sizeof(s->last_date_buf) - 1);
    s->last_date_buf[sizeof(s->last_date_buf) - 1] = '\0';

    lv_label_set_text(s->time_label, time_buf);
    lv_label_set_text(s->ampm_label, ampm_buf);
    lv_label_set_text(s->date_label, date_buf);
    // No local time yet (e.g. after a cold power cycle, before the first Orion or
    // iPhone time sync): hide the WHOLE clock — time, "·" separator and date —
    // rather than showing a "--:--" placeholder.
    if (clock_set) {
        lv_obj_clear_flag(s->time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s->sep_label,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s->date_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s->time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->sep_label,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s->date_label, LV_OBJ_FLAG_HIDDEN);
    }
    // ampm_label is hidden in 24-hour format too (ampm_buf empty) — not just
    // when the clock is unset — so time_row's gap doesn't leave a blank slot.
    if (clock_set && ampm_buf[0] != '\0') {
        lv_obj_clear_flag(s->ampm_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s->ampm_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// UID of a just-arrived notification whose tile should animate in once. Set by
// note_new_notification(), consumed + cleared by the next refresh(). A separate
// pending flag is needed because UID 0 is a valid ANCS UID (can't use 0 as the
// "nothing pending" sentinel).
static uint32_t g_anim_uid     = 0;
static bool     g_anim_pending = false;

static void tile_opa_cb(void* o, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(o), (lv_opa_t)v, 0);
}
static void tile_scale_cb(void* o, int32_t v) {
    lv_obj_set_style_transform_scale_x(static_cast<lv_obj_t*>(o), v, 0);
    lv_obj_set_style_transform_scale_y(static_cast<lv_obj_t*>(o), v, 0);
}

// One-shot entrance: a genuinely-new icon fades + pops in from ~62% scale,
// pivoting about its centre. (256 = 100% in LVGL's transform scale units.)
static void animate_tile_in(lv_obj_t* tile) {
    lv_obj_set_style_transform_pivot_x(tile, ICON_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(tile, ICON_SIZE / 2, 0);
    lv_obj_set_style_opa(tile, LV_OPA_TRANSP, 0);
    tile_scale_cb(tile, 160);

    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, tile);
    lv_anim_set_time(&a, 240);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, tile_opa_cb);
    lv_anim_start(&a);

    lv_anim_t b; lv_anim_init(&b);
    lv_anim_set_var(&b, tile);
    lv_anim_set_time(&b, 240);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_values(&b, 160, 256);
    lv_anim_set_exec_cb(&b, tile_scale_cb);
    lv_anim_start(&b);
}

// Shared shell for the two 26 px corner badges make_ancs_tile() can draw on a
// tile (stacked-count pill and silent-bell dot): fixed square size (so
// LV_RADIUS_CIRCLE renders a true circle — a content-sized single-digit box
// is taller than wide, which would turn into a pill, not a round badge), the
// same black 2 px cutout border, no shadow/padding, non-scrollable and
// non-clickable (so taps pass through to the tile), aligned to a tile
// corner. Caller fills in the bg colour and the centred content (label or
// icon) on the returned object.
lv_obj_t* make_corner_badge(lv_obj_t* tile, lv_color_t bg_color, lv_align_t align) {
    constexpr int BADGE_SIZE = 26;
    lv_obj_t* badge = lv_obj_create(tile);
    lv_obj_set_size(badge, BADGE_SIZE, BADGE_SIZE);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(badge, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(badge, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(badge, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
    lv_obj_clear_flag(badge, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_align(badge, align, 0, 0);
    return badge;
}

// ANCS icon tile. Uses a compiled-in raster asset when available (12 px radius,
// matching the HTML prototype); falls back to a solid brand-colour circle.
// `token` selects the icon; `uid` identifies the exact (most-recent) notification
// for this app; `count` is the number of stacked notifications — when > 1, a
// small red badge is drawn in the top-right corner showing the count.
lv_obj_t* make_ancs_tile(lv_obj_t* parent, const char* token, uint32_t uid, uint8_t count = 1) {
    lv_obj_t* tile = lv_obj_create(parent);
    lv_obj_set_size(tile, ICON_SIZE, ICON_SIZE);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(tile, LV_OPA_60, LV_STATE_PRESSED);

    // Brand icon if we have one; otherwise a fallback chosen by ANCS category
    // (so an unknown email app shows an envelope, an unknown social app a chat
    // bubble, etc.), falling back to the generic bell for unknown categories.
    const lv_image_dsc_t* img = ancs_icons::image(token);
    if (!img) img = ancs_icons::category_image(app_state::ancs_category(uid));
    if (img) {
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(tile, 12, LV_PART_MAIN);
        lv_obj_t* img_obj = lv_image_create(tile);
        lv_image_set_src(img_obj, img);
        lv_obj_center(img_obj);
        lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_set_style_bg_color(tile, theme::color(ancs_icons::color(token)), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(tile, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    }

    // Category / importance accent ring: a call = solid yellow while still
    // RINGING, solid red once ANSWERED/active (modal_incoming_call's session
    // is the authoritative "has this call been answered" state — it's what
    // starts running the instant ancs_client sees the ANCS positive action
    // disappear, same signal the in-call dialog itself swaps on); otherwise
    // an Important notification = accent. Overrides the default 0-width
    // border. Solid (LV_OPA_COVER, no animation) by design — Orion mirrors
    // this exact yellow/red pair on its own call chip (pc-app.md).
    uint8_t cat = app_state::ancs_category(uid);
    bool is_call = (cat == app_state::AncsCategory::INCOMING_CALL ||
                    cat == app_state::AncsCategory::ACTIVE_CALL);
    if (is_call) {
        uint32_t active_uid = 0;
        bool     answered   = modal_incoming_call::session_state(&active_uid, nullptr) &&
                              active_uid == uid;
        lv_obj_set_style_border_width(tile, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, theme::color(
            answered ? theme::COLOR_DANGER : theme::COLOR_CALL_RINGING), LV_PART_MAIN);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    } else if (app_state::ancs_is_important(uid)) {
        lv_obj_set_style_border_width(tile, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, theme::color(theme::COLOR_ACCENT), LV_PART_MAIN);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    }

    // Tap → call notifications reopen the in-call dialog (caller + live timer +
    // End call); everything else opens the generic ANCS detail overlay. Shared
    // dispatcher (modal_ancs_notification::open_for_uid) also used by the
    // iPhone Info modal's drill-down lists.
    lv_obj_set_user_data(tile, reinterpret_cast<void*>((uintptr_t)uid));
    lv_obj_add_event_cb(tile, [](lv_event_t* e) {
        lv_obj_t* t = (lv_obj_t*)lv_event_get_current_target(e);
        uint32_t uid = (uint32_t)(uintptr_t)lv_obj_get_user_data(t);
        modal_ancs_notification::open_for_uid(lv_screen_active(), uid);
    }, LV_EVENT_CLICKED, nullptr);

    // Genuinely-new notification (flagged via note_new_notification) pops in.
    if (g_anim_pending && uid == g_anim_uid) animate_tile_in(tile);

    // Stacked-count badge: shown when multiple notifications from the same app
    // are queued. Red pill in the top-right corner; text is the count or "9+"
    // for ten or more. Badge is non-clickable so taps pass through to the tile.
    if (count > 1) {
        char buf[4];
        if (count > 9) {
            buf[0] = '9'; buf[1] = '+'; buf[2] = '\0';
        } else {
            buf[0] = static_cast<char>('0' + count); buf[1] = '\0';
        }
        lv_obj_t* badge = make_corner_badge(tile, theme::color(theme::COLOR_DANGER), LV_ALIGN_TOP_RIGHT);

        lv_obj_t* lbl = lv_label_create(badge);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, theme::font_body(), LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_black(), LV_PART_MAIN);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(lbl);
    } else if (app_state::ancs_notification_by_uid(uid).silent) {
        // Silent indicator — small icon-only badge (bell-off glyph), top-left
        // corner. Same 26px circle + card-coloured cutout border and the same
        // "count wins over silent" precedence as modal_ancs_list.cpp's row
        // badge (a stacked tile's count badge already occupies the opposite
        // corner, but showing silent here too would misrepresent an entire
        // stack as silent when only its representative notification is) —
        // top-left placement matches that list and modal_ancs_notification's
        // detail overlay, so the glyph means the same thing everywhere.
        // Same black 2px border as the count badge above (this tile sits
        // directly on the pure-black screen background, so a plain black
        // border reads the same as the card-coloured "cutout" trick the
        // modal badges use on their dark-grey card background).
        lv_obj_t* badge = make_corner_badge(tile, lv_color_hex(0x2A2A2A), LV_ALIGN_TOP_LEFT);

        lv_obj_t* icon = lv_image_create(badge);
        lv_image_set_src(icon, ancs_badge_icons::silent());
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(icon);
        lv_obj_set_style_image_recolor(icon, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    }

    return tile;
}

// Phone glyph — drawn from primitives. Includes the top mic strip and
// the bottom home-button dot. Always the neutral (secondary text) colour
// in both states — connection state is conveyed by a diagonal slash cut
// across the glyph when disconnected, not by a colour swap, so the icon
// reads the same whether or not the viewer can distinguish red from grey.
// The icon is a permanent status-bar button either way (tap → unpair when
// connected / re-pair when not).
//
// Layout inside the 64 px square:
//
//          mic strip  (a short horizontal line near the top of the body)
//         ┌──────────┐
//        ╲│          │
//         │          │╲   diagonal slash (disconnected only)
//         │          │
//         │    ·     │   home-button dot
//         └──────────┘
//
// Rebuilds the glyph children inside `root` for the given connection state.
// `root` keeps its size, flags, and event callbacks across rebuilds.
void build_phone_glyph(lv_obj_t* root, bool connected) {
    lv_obj_clean(root);

    const lv_color_t color = theme::color(theme::COLOR_TEXT_SECONDARY);

    // Phone body outline. ~28×46 px centred in the 64 px box.
    lv_obj_t* body = lv_obj_create(root);
    lv_obj_set_size(body, 28, 46);
    lv_obj_center(body);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, color, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(body, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // Top mic / speaker strip — short horizontal line ~3 px below the top
    // of the body. Implemented as a thin rounded rect (no rotation, so no
    // layer-transparency requirement).
    lv_obj_t* mic = lv_obj_create(body);
    lv_obj_set_size(mic, 14, 3);
    lv_obj_align(mic, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_bg_color(mic, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mic, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(mic, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(mic, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mic, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mic, LV_OBJ_FLAG_CLICKABLE);

    // Home-button dot near the bottom of the body.
    lv_obj_t* home = lv_obj_create(body);
    lv_obj_set_size(home, 4, 4);
    lv_obj_align(home, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(home, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(home, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(home, 0, LV_PART_MAIN);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(home, LV_OBJ_FLAG_CLICKABLE);

    if (!connected) {
        // Diagonal slash cut across the whole 64 px icon box — the sole
        // signal for "disconnected" now that colour no longer changes.
        // Drawn on `root` (not `body`) so it crosses the full glyph, same
        // corner-to-corner ratio as the web prototype's mask-cut version
        // (Ori_UI_Prototype.html's #i-phone-broken).
        static const lv_point_precise_t slash_pts[] = {{8, 56}, {56, 8}};
        lv_obj_t* slash = lv_line_create(root);
        lv_line_set_points(slash, slash_pts, 2);
        lv_obj_set_style_line_width(slash, 4, 0);
        lv_obj_set_style_line_color(slash, color, 0);
        lv_obj_set_style_line_rounded(slash, true, 0);
        lv_obj_clear_flag(slash, LV_OBJ_FLAG_CLICKABLE);
    }
}

lv_obj_t* make_phone_icon(lv_obj_t* parent) {
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, PHONE_SIZE, PHONE_SIZE);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(root, LV_OPA_60, LV_STATE_PRESSED);

    build_phone_glyph(root, /*connected=*/false);
    return root;
}

// Headphones glyph drawn from LVGL primitives — a curved headband + two
// rounded ear cups. Sized for the 60-px mode-toggle button. Mirrors the
// `i-controls` SVG in the HTML prototype.
//
// Band sizing math: the arc bounding box is 28x28. With angles 180..360
// (top half only) the arc's two end-points sit on the box's horizontal
// midline at x = box_center ± 14. We align the box so the midline lands
// at y_btn_center + 1 — exactly where the ear-cup tops are — and so the
// two arc end-points sit directly above the ear-cup centres (also at
// x_btn_center ± 14). That makes the band visually flow into the cups
// instead of floating above them.
lv_obj_t* make_headphones_glyph(lv_obj_t* parent, uint32_t color) {
    // Headband arc — an LVGL Arc primitive set to a top semicircle.
    lv_obj_t* band = lv_arc_create(parent);
    lv_obj_set_size(band, 28, 28);
    lv_obj_align(band, LV_ALIGN_CENTER, 0, +1);
    lv_arc_set_bg_angles(band, 180, 360);     // top half only
    lv_arc_set_value(band, 100);
    lv_obj_remove_style(band, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(band, theme::color(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(band, theme::color(color), LV_PART_MAIN);
    lv_obj_set_style_arc_width(band, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(band, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(band, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(band, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    // Ear cups — identical 7x11 rounded rects, mirrored left/right of centre.
    auto make_cup = [&](int16_t dx) {
        lv_obj_t* cup = lv_obj_create(parent);
        lv_obj_set_size(cup, 7, 11);
        lv_obj_align(cup, LV_ALIGN_CENTER, dx, 7);
        lv_obj_set_style_bg_color(cup, theme::color(color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cup, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(cup, 3, LV_PART_MAIN);
        lv_obj_set_style_border_width(cup, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cup, 0, LV_PART_MAIN);
        lv_obj_clear_flag(cup, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(cup, LV_OBJ_FLAG_SCROLLABLE);
    };
    make_cup(-14);  // left ear cup
    make_cup(14);   // right ear cup

    return band;  // returns one of the children; caller does not need it
}

// Calendar glyph drawn from LVGL primitives — matches the `i-cal` SVG symbol
// used in both the HTML prototype and screen_no_meetings.cpp's make_cal_glyph().
//
// The SVG (viewBox 24×24) has three pieces:
//   1. Hollow rounded body rect  — SVG rect x=3,y=5,w=18,h=16,rx=2
//   2. Thin horizontal divider   — SVG path M3 9 h18  (at y=9 inside the body)
//   3. Two vertical ring tabs    — SVG paths M8 3v4 / M16 3v4
//      (tabs straddle the body top edge: start above y=5, end inside the body)
//
// Scaled to ~2.5× to fill the 60×60 mode-toggle button proportionally.
// Layout (all coords relative to the transparent 60×60 wrapper):
//
//           x=18  x=38
//      y=8  |  |  |  |   ← tabs start (above body top)
//      y=13 └──┴──┴──┘   ← body top edge (tabs straddle this)
//            [  body  ]
//      y=23 ├──────────┤  ← horizontal divider
//            [  body  ]
//      y=53 └──────────┘  ← body bottom
//
// Tab x-centres (SVG x=8 and x=16 at 2.5×): 20 px and 40 px.
// Tab: 4 px wide, 10 px tall, top at y=8 (5 px above body top at y=13),
//      bottom at y=18 (5 px into the body — just below the divider line).
lv_obj_t* make_calendar_glyph(lv_obj_t* parent, uint32_t color) {
    // Transparent 60×60 positioning wrapper — same pattern as make_cal_glyph()
    // in screen_no_meetings.cpp. Children use lv_obj_set_pos() for exact pixel
    // placement without relying on LV_ALIGN_CENTER offset arithmetic.
    lv_obj_t* glyph = lv_obj_create(parent);
    lv_obj_set_size(glyph, 60, 60);
    lv_obj_align(glyph, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(glyph, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(glyph, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(glyph, 0, LV_PART_MAIN);
    lv_obj_clear_flag(glyph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(glyph, LV_OBJ_FLAG_CLICKABLE);

    const lv_color_t c = theme::color(color);

    // 1) Body — hollow rounded rect. 45×40 px at pos (8, 13), radius 5.
    //    SVG: x=3,y=5,w=18,h=16,rx=2  →  ×2.5: w=45,h=40, rx=5.
    //    Horizontally centred in 60 px: x = (60-45)/2 = 7 → rounded to 8.
    lv_obj_t* body = lv_obj_create(glyph);
    lv_obj_set_size(body, 45, 40);
    lv_obj_set_pos(body, 8, 13);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, c, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(body, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 2) Horizontal divider — full body width, 3 px tall.
    //    SVG: M3 9 h18 (y=9 → 4 units below body top at y=5 → 4×2.5=10 px into body).
    //    Absolute y = body_top + 10 = 13 + 10 = 23.
    lv_obj_t* divider = lv_obj_create(glyph);
    lv_obj_set_size(divider, 45, 3);
    lv_obj_set_pos(divider, 8, 23);
    lv_obj_set_style_bg_color(divider, c, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(divider, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    // 3) Two vertical ring tabs straddling the body's top edge.
    //    SVG: M8 3v4 and M16 3v4  (y=3..7, width = stroke-width 1.6 → 4 px at 2.5×).
    //    Tab x-centres at SVG x=8 and x=16 → 8×2.5=20 and 16×2.5=40 absolute.
    //    Tab top: SVG y=3 → 3×2.5=7.5 → y=8. Tab height: 4×2.5=10 px.
    auto make_tab = [&](int16_t cx_abs) {
        lv_obj_t* tab = lv_obj_create(glyph);
        lv_obj_set_size(tab, 4, 10);
        lv_obj_set_pos(tab, cx_abs - 2, 8);
        lv_obj_set_style_bg_color(tab, c, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(tab, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(tab, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_CLICKABLE);
    };
    make_tab(20);   // left ring tab — SVG x=8 × 2.5
    make_tab(40);   // right ring tab — SVG x=16 × 2.5

    return glyph;
}

// Defer a callback by 1 ms via a one-shot timer. Calling state-machine
// functions directly from within LVGL's event dispatch stack overflows the
// loopTask stack (NVS flash write + full screen rebuild on top of the event
// depth) — every tap handler below routes through this instead. `slot` is
// the caller's own static storage (each tap source keeps its own, so two
// taps arriving in the same 1 ms window can't clobber each other's pending
// callback) and must outlive the timer, which it does since all slots are
// static file-scope std::function objects.
void defer_call(std::function<void()>* slot, std::function<void()> cb) {
    *slot = std::move(cb);
    lv_timer_t* t = lv_timer_create([](lv_timer_t* t) {
        auto* slot = static_cast<std::function<void()>*>(lv_timer_get_user_data(t));
        lv_timer_delete(t);
        if (*slot) {
            auto cb = std::move(*slot);
            *slot = nullptr;
            cb();
        }
    }, 1, slot);
    lv_timer_set_repeat_count(t, 1);
}

// Deferred callback for the phone-disconnect icon tap — see defer_call().
static std::function<void()> s_deferred_phone_cb;

void on_phone_icon_tap(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;
    auto* st = static_cast<StatusBarState*>(lv_event_get_user_data(e));
    if (!st) return;
    // Routing is by BOND state, not connection state:
    //   bonded (connected or not) → iPhone Info/Stats overlay. We already know
    //                               this iPhone, so show name/status/signal/
    //                               notification stats; its own Unpair button
    //                               hands off to modal_unpair_phone, which
    //                               wipes the bond via on_unpair_phone() (opens
    //                               the slot + re-advertises ANCS).
    //   not bonded                → re-pair screen so a fresh iPhone can bond.
    bool bonded    = st->phone_bonded;
    bool connected = st->phone_connected;
    lv_obj_t* screen = lv_screen_active();
    defer_call(&s_deferred_phone_cb, [bonded, connected, screen]() {
        if (bonded) {
            modal_iphone_info::create(screen, connected);
        } else {
            lv_obj_t* pairing = screen_setup::create(screen_setup::Step::PhonePairing, screen);
            lv_scr_load_anim(pairing, LV_SCR_LOAD_ANIM_NONE, 0, 0, /*auto_del=*/false);
        }
    });
}

constexpr int16_t MODE_TOGGLE_SIZE = 60;

// Re-draw the mode-toggle visuals to reflect the current mode.
// Icon always shows the action you'll perform on tap:
//   Calendar mode → headphones glyph, neutral bg    ("tap to enter Media")
//   Clock mode    → calendar glyph,   neutral bg    ("tap to return to previous mode")
//   Media mode    → calendar glyph,   accent-tinted ("tap to return to Calendar")
void rebuild_mode_toggle_glyph(lv_obj_t* btn, widget_status_bar::Mode mode) {
    lv_obj_clean(btn);
    if (mode == widget_status_bar::Mode::Calendar) {
        lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_ELEV), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_DIVIDER), LV_PART_MAIN);
        make_headphones_glyph(btn, theme::COLOR_TEXT_PRIMARY);
    } else if (mode == widget_status_bar::Mode::Clock) {
        // In Clock: show calendar icon (neutral) — "return to previous mode".
        lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_ELEV), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_DIVIDER), LV_PART_MAIN);
        make_calendar_glyph(btn, theme::COLOR_TEXT_PRIMARY);
    } else {
        // Media mode active — show calendar icon, accent-tinted.
        lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_ACCENT_SOFT), LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, theme::color(theme::COLOR_ACCENT_LINE), LV_PART_MAIN);
        make_calendar_glyph(btn, theme::COLOR_ACCENT);
    }
}

// Deferred callbacks — see defer_call() above.
static std::function<void()> s_deferred_toggle_cb;
static std::function<void()> s_deferred_time_tap_cb;

void on_mode_toggle_tap(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    auto* state = static_cast<StatusBarState*>(lv_event_get_user_data(e));
    if (!state || !state->mode_cb) return;
    defer_call(&s_deferred_toggle_cb, state->mode_cb);
}

void on_time_tap(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    auto* state = static_cast<StatusBarState*>(lv_event_get_user_data(e));
    if (!state) return;
    // A long-press already fired Calendar entry for this same press — LVGL
    // would otherwise still dispatch CLICKED on release (its own long-press
    // threshold is the indev-wide 3 s set in main.cpp; ours fires at 1 s, so
    // by release time LVGL's internal long_pr_sent flag is still false).
    if (state->time_long_press_fired) {
        state->time_long_press_fired = false;
        return;
    }
    if (!state->time_tap_cb) return;
    defer_call(&s_deferred_time_tap_cb, state->time_tap_cb);
}

// Custom-duration press-and-hold for the date+time block only. The shared
// LVGL indev long-press threshold (set once, globally, in main.cpp) is 3 s —
// tuned for the profile-photo factory-reset gesture and the phone icon. The
// Calendar entry gesture wants a shorter 1 s hold, so it can't reuse
// LV_EVENT_LONG_PRESSED (indev-wide) and instead times the press itself via
// LV_EVENT_PRESSED/RELEASED/PRESS_LOST and a one-shot lv_timer.
constexpr uint32_t TIME_LONG_PRESS_MS = 1000;

void on_time_press_start(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    auto* state = static_cast<StatusBarState*>(lv_event_get_user_data(e));
    if (!state) return;
    if (state->time_long_press_timer) {
        lv_timer_delete(state->time_long_press_timer);
        state->time_long_press_timer = nullptr;
    }
    state->time_long_press_fired = false;
    if (!state->time_long_press_cb) return;
    state->time_long_press_timer = lv_timer_create([](lv_timer_t* t) {
        auto* st = static_cast<StatusBarState*>(lv_timer_get_user_data(t));
        lv_timer_delete(t);
        st->time_long_press_timer = nullptr;
        st->time_long_press_fired = true;
        if (st->time_long_press_cb) st->time_long_press_cb();
    }, TIME_LONG_PRESS_MS, state);
    lv_timer_set_repeat_count(state->time_long_press_timer, 1);
}

void on_time_press_end(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    auto* state = static_cast<StatusBarState*>(lv_event_get_user_data(e));
    if (!state || !state->time_long_press_timer) return;
    lv_timer_delete(state->time_long_press_timer);
    state->time_long_press_timer = nullptr;
}

lv_obj_t* make_mode_toggle(lv_obj_t* parent, StatusBarState* state) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_size(btn, MODE_TOGGLE_SIZE, MODE_TOGGLE_SIZE);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(btn, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, on_mode_toggle_tap, LV_EVENT_CLICKED, state);
    rebuild_mode_toggle_glyph(btn, state->mode);
    return btn;
}

} // namespace

namespace widget_status_bar {

// Weak reference to the most-recently-created bar — used by refresh_active()
// so the ANCS client can push icon updates immediately without knowing which
// screen owns the bar. Cleared to nullptr on LV_EVENT_DELETE.
lv_obj_t* g_active_bar = nullptr;

// Module-level defaults — set via set_default_* helpers. Newly-created status
// bars read these so screen_manager can change state once per screen-switch.
bool g_default_pc_connected   = true;
bool g_default_phone_bonded   = false;
bool g_default_phone_connected = false;
Mode g_default_mode           = Mode::Calendar;
ModeToggleCb g_default_mode_cb  = nullptr;
TimeTapCb    g_default_time_tap_cb = nullptr;
TimeLongPressCb g_default_time_long_press_cb = nullptr;

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_obj_get_width(parent), HEIGHT);
    lv_obj_set_pos(bar, 0, 0);

    lv_obj_set_style_bg_color(bar, theme::color(theme::COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bar, PAD_X, LV_PART_MAIN);
    lv_obj_set_style_pad_right(bar, PAD_X, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, theme::color(theme::COLOR_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 3, LV_PART_MAIN);  // 3 px per screen-layout.md
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* state = new StatusBarState();
    state->show_datetime     = true;
    state->pc_connected      = g_default_pc_connected;
    state->phone_bonded      = g_default_phone_bonded;
    state->phone_connected   = g_default_phone_connected;
    state->phone_glyph_state = -1;  // unset → first set_phone_connected draws it
    state->mode              = g_default_mode;
    state->mode_cb        = g_default_mode_cb;
    state->time_tap_cb    = g_default_time_tap_cb;
    state->time_long_press_cb = g_default_time_long_press_cb;
    state->time_long_press_timer = nullptr;
    state->time_long_press_fired = false;
    state->mode_toggle    = nullptr;

    // ===== Date/time block — tappable to enter Clock mode =====
    state->datetime_row = lv_obj_create(bar);
    lv_obj_set_size(state->datetime_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(state->datetime_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->datetime_row, 0, 0);
    lv_obj_set_style_pad_all(state->datetime_row, 0, 0);
    lv_obj_clear_flag(state->datetime_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(state->datetime_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(state->datetime_row, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(state->datetime_row, on_time_tap, LV_EVENT_CLICKED, state);
    lv_obj_add_event_cb(state->datetime_row, on_time_press_start, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(state->datetime_row, on_time_press_end, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(state->datetime_row, on_time_press_end, LV_EVENT_PRESS_LOST, state);
    lv_obj_set_flex_flow(state->datetime_row, LV_FLEX_FLOW_ROW);
    // Cross-axis CENTER so the 30 px time and 22 px sep/date are vertically
    // centered to each other; the row as a whole is then centered in the
    // 84 px bar by the parent's LV_FLEX_ALIGN_CENTER.
    lv_obj_set_flex_align(state->datetime_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state->datetime_row, DATETIME_GAP, 0);

    // time_row wraps time_label + ampm_label with a tight 4 px gap — much
    // closer than DATETIME_GAP, so "2:30 PM" reads as one unit rather than
    // three evenly-spaced blocks. Not clickable itself (lv_obj_create()
    // defaults to clickable) so taps still bubble to datetime_row's handlers.
    state->time_row = lv_obj_create(state->datetime_row);
    lv_obj_set_size(state->time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(state->time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->time_row, 0, 0);
    lv_obj_set_style_pad_all(state->time_row, 0, 0);
    lv_obj_clear_flag(state->time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(state->time_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(state->time_row, LV_FLEX_FLOW_ROW);
    // Cross-axis CENTER so the smaller AM/PM subtext is vertically centered
    // against the larger time digits.
    lv_obj_set_flex_align(state->time_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state->time_row, 4, 0);

    state->time_label = lv_label_create(state->time_row);
    lv_obj_set_style_text_color(state->time_label, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(state->time_label, theme::font_time(), 0);

    // AM/PM subtext — smaller than the hour:minute text; hidden entirely in
    // 24-hour format (update_clock_labels() toggles this every second).
    state->ampm_label = lv_label_create(state->time_row);
    lv_obj_set_style_text_color(state->ampm_label, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(state->ampm_label, theme::font_body(), 0);
    lv_obj_add_flag(state->ampm_label, LV_OBJ_FLAG_HIDDEN); // shown by the first update_clock_labels() tick if 12h

    state->sep_label = lv_label_create(state->datetime_row);
    lv_label_set_text_static(state->sep_label, "\xC2\xB7");  // U+00B7 MIDDLE DOT, static literal
    lv_obj_set_style_text_color(state->sep_label, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(state->sep_label, theme::font_meta(), 0);

    state->date_label = lv_label_create(state->datetime_row);
    lv_obj_set_style_text_color(state->date_label, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(state->date_label, theme::font_meta(), 0);

    // ===== Spacer (flex-grow) =====
    lv_obj_t* spacer = lv_obj_create(bar);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    // ===== Right cluster: ANCS row, phone-disconnect, mode-toggle =====
    //
    // These three sit directly in the bar's flex (not nested in a
    // `right_row` container) so the bar's flex algorithm sees all of
    // them up-front and can size + position them correctly even as
    // visibility toggles at runtime. An earlier nested-container design
    // relied on LV_SIZE_CONTENT to re-sum children when visibility
    // changed; LVGL's flex did not propagate that reliably and the
    // phone-disconnect glyph ended up stacked on top of the mode-toggle
    // in the same flex cell. Putting them in the bar directly lets the
    // bar's flex use the spacer's flex_grow to absorb the slack.
    //
    // `ancs_row` is the only sub-container that needs its own flex — for
    // the variable number of ANCS tiles with the right inter-tile gap.
    state->ancs_row = lv_obj_create(bar);
    // Width is recomputed by refresh() based on the actual tile count
    // (LV_SIZE_CONTENT does NOT re-expand reliably after lv_obj_delete_children()).
    // Start at 0 — refresh() runs at the end of create() and sets the
    // right value before the bar is laid out.
    lv_obj_set_size(state->ancs_row, 0, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(state->ancs_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->ancs_row, 0, 0);
    lv_obj_set_style_pad_all(state->ancs_row, 0, 0);
    lv_obj_clear_flag(state->ancs_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(state->ancs_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state->ancs_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state->ancs_row, ICON_GAP, 0);

    // Phone icon — direct child of bar, between ANCS and mode-toggle.
    // Always visible; set_phone_connected() recolours it (neutral when
    // connected, danger red when disconnected).
    state->phone_icon = make_phone_icon(bar);


    // Tap or long-press on the phone-disconnect icon. Deferred via 1ms timer —
    // same pattern as on_mode_toggle_tap/on_time_tap: calling lv_scr_load_anim
    // or creating a full screen from inside LVGL event dispatch overflows the
    // loopTask stack and crashes.
    lv_obj_add_event_cb(state->phone_icon, on_phone_icon_tap, LV_EVENT_CLICKED, state);
    lv_obj_add_event_cb(state->phone_icon, on_phone_icon_tap, LV_EVENT_LONG_PRESSED, state);

    // user_data must be set before any public setter that reads it via
    // lv_obj_get_user_data() — set_phone_connected() returns early if null.
    lv_obj_set_user_data(bar, state);

    // Paint the glyph colour + ANCS-row visibility for the inherited connection
    // state (make_phone_icon only draws the disconnected glyph).
    set_phone_connected(bar, state->phone_connected);

    // Mode-toggle — direct child of bar, rightmost when visible.
    state->mode_toggle = make_mode_toggle(bar, state);
    if (!state->pc_connected) {
        lv_obj_add_flag(state->mode_toggle, LV_OBJ_FLAG_HIDDEN);
    }
    // 16 px column gap between bar children (datetime, spacer, ancs,
    // phone, mode). Spacer takes flex_grow=1 → absorbs slack so the
    // right cluster stays anchored to the right edge.
    lv_obj_set_style_pad_column(bar, 16, 0);

    // 1-second timer keeps the clock live without needing the state machine
    // to call refresh() externally. Deleted explicitly on LV_EVENT_DELETE
    // because lv_timer_create() produces a free-standing timer, not a child.
    state->clock_timer = lv_timer_create([](lv_timer_t* t) {
        auto* b = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
        update_clock_labels(b);   // labels only — ANCS row rebuilds on data change
    }, 1000, bar);

    g_active_bar = bar;
    lv_obj_add_event_cb(bar, [](lv_event_t* e) {
        auto* st = static_cast<StatusBarState*>(lv_event_get_user_data(e));
        if (st->clock_timer) { lv_timer_delete(st->clock_timer); st->clock_timer = nullptr; }
        if (st->time_long_press_timer) { lv_timer_delete(st->time_long_press_timer); st->time_long_press_timer = nullptr; }
        if (g_active_bar == (lv_obj_t*)lv_event_get_target(e)) g_active_bar = nullptr;
        delete st;
    }, LV_EVENT_DELETE, state);

    refresh(bar);
    return bar;
}

void set_show_datetime(lv_obj_t* bar, bool show) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->show_datetime = show;
    if (show) lv_obj_clear_flag(s->datetime_row, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(s->datetime_row, LV_OBJ_FLAG_HIDDEN);
}

void set_phone_connected(lv_obj_t* bar, bool connected) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;

    s->phone_connected = connected;

    // ANCS icons only make sense while the phone link is up; the phone icon
    // itself stays visible in both states and just gains/loses its
    // diagonal slash.
    if (connected) lv_obj_clear_flag(s->ancs_row, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(s->ancs_row, LV_OBJ_FLAG_HIDDEN);

    // refresh() calls this every second — only rebuild the glyph when the
    // state actually changed.
    if (s->phone_glyph_state != (int8_t)connected) {
        s->phone_glyph_state = (int8_t)connected;
        build_phone_glyph(s->phone_icon, connected);
    }
}

void set_pc_connected(lv_obj_t* bar, bool connected) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->pc_connected = connected;
    if (!s->mode_toggle) return;
    if (connected) lv_obj_clear_flag(s->mode_toggle, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(s->mode_toggle,  LV_OBJ_FLAG_HIDDEN);
}

void set_mode(lv_obj_t* bar, Mode mode) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->mode = mode;
    if (s->mode_toggle) rebuild_mode_toggle_glyph(s->mode_toggle, mode);
}

void set_mode_toggle_cb(lv_obj_t* bar, ModeToggleCb cb) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->mode_cb = cb;
}

void set_time_tap_cb(lv_obj_t* bar, TimeTapCb cb) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->time_tap_cb = cb;
}

void set_time_long_press_cb(lv_obj_t* bar, TimeLongPressCb cb) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->time_long_press_cb = cb;
}

void set_phone_bonded(lv_obj_t* bar, bool bonded) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;
    s->phone_bonded = bonded;
}

void set_default_pc_connected(bool connected)      { g_default_pc_connected = connected; }
void set_default_phone_bonded(bool bonded)          { g_default_phone_bonded = bonded; }
void set_default_phone_connected(bool connected)    { g_default_phone_connected = connected; }

void set_all_phone_bonded(bool bonded) {
    g_default_phone_bonded = bonded;
    if (g_active_bar) {
        auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(g_active_bar));
        if (s) s->phone_bonded = bonded;
    }
}

void set_all_phone_connected(bool connected) {
    g_default_phone_connected = connected;
    if (g_active_bar) set_phone_connected(g_active_bar, connected);
}

void set_default_mode(Mode mode)                    { g_default_mode = mode; }
void set_default_mode_toggle_cb(ModeToggleCb cb)    { g_default_mode_cb = cb; }
void set_default_time_tap_cb(TimeTapCb cb)          { g_default_time_tap_cb = cb; }
void set_default_time_long_press_cb(TimeLongPressCb cb) { g_default_time_long_press_cb = cb; }

void refresh_active() {
    if (g_active_bar) refresh(g_active_bar);
}

void note_new_notification(uint32_t uid) { g_anim_uid = uid; g_anim_pending = true; }

void refresh(lv_obj_t* bar) {
    auto* s = static_cast<StatusBarState*>(lv_obj_get_user_data(bar));
    if (!s) return;

    update_clock_labels(bar);

    // Rebuild ANCS row from current config. Gate on the bar's own
    // phone_connected field — the authoritative BLE link state set by
    // set_all_phone_connected() — NOT cfg.phone_connected, which only goes
    // true once a notification is queued. (Driving the icon from cfg caused
    // the phone icon to read "disconnected" in the window between iPhone
    // connect and the first notification, so a tap wiped a live bond.)
    const auto& cfg = app_state::ancs_config();
    lv_obj_clean(s->ancs_row);
    size_t visible_tiles = 0;
    if (s->phone_connected) {
        // Queue is oldest → newest. Show the NEWEST MAX_ANCS_ICONS: start at
        // count-5 (or 0). Adding oldest-of-window first puts it leftmost and the
        // newest rightmost (flex row, left→right). Reading one removes it and
        // this re-windows — the next-newest slides to the right, and any hidden
        // older notification slides into view on the left.
        size_t start = (cfg.count > app_state::MAX_ANCS_ICONS)
                       ? cfg.count - app_state::MAX_ANCS_ICONS
                       : 0;
        for (size_t i = start; i < cfg.count && i < app_state::MAX_ANCS_NOTIFICATIONS; ++i) {
            if (cfg.icons[i]) {
                make_ancs_tile(s->ancs_row, cfg.icons[i], cfg.uids[i], cfg.counts[i]);
                ++visible_tiles;
            }
        }
    }
    // Size the ANCS row to exactly its tile count so the bar's flex
    // doesn't overshoot and push the mode-toggle off the right edge.
    // (LV_SIZE_CONTENT does not re-expand the row after lv_obj_delete_children()
    // in LVGL 8, so we compute the width explicitly.)
    int ancs_w = (visible_tiles == 0)
                 ? 0
                 : (int)visible_tiles * ICON_SIZE + ((int)visible_tiles - 1) * ICON_GAP;
    lv_obj_set_width(s->ancs_row, ancs_w);

    // One-shot: the entrance animation (if any) has now been applied.
    g_anim_pending = false;
}

} // namespace widget_status_bar
