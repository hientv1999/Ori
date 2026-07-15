#include "screens/screen_calendar.h"

#include <lvgl.h>
#include <stdio.h>
#include <time.h>

#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Calendar (month view) screen — left panel only, entered by long-pressing
// the status-bar time. Status bar is present but its date/time is hidden,
// same as screen_clock. View-only: a 7-column month grid with today
// highlighted in an accent-filled circle, and left/right chevrons in the
// header to navigate months. Mode-toggle acts as a return button.
//
// Navigation re-renders just the left-panel subtree (not a full screen
// reload via state_machine) — cheap enough to run directly from the
// LVGL click event, same as widget_status_bar::refresh()'s ANCS-row rebuild.

namespace {

// Month/year currently being viewed. -1 = uninitialized; create() falls
// back to "today" if reset_view() was never called (defensive only — the
// real entry path, state_machine::on_calendar_enter(), always calls it).
int g_view_year  = -1;
int g_view_month = -1; // 0-based (0 = January)

// Left-panel container of the currently-active calendar screen, if any.
// Nav button callbacks rebuild into this; cleared on screen delete.
lv_obj_t* g_left_panel = nullptr;

// Cache of "today" as a noon-normalized time_t, keyed by (year, month, day).
// render_into() reruns on every month/year nav tap and refresh_today(), but
// the mktime() needed to place today's cell in the grid only ever changes
// once every 24h — recompute it only when the wall-clock day actually rolls
// over instead of on every render.
int g_cached_today_year  = -1;
int g_cached_today_month = -1;
int g_cached_today_day   = -1;
time_t g_cached_today_tt = 0;

constexpr int16_t BADGE_SIZE = 44; // sized so a 6-week month's rows stay >= the badge (no clipping)
constexpr int16_t NAV_BTN_SIZE = 36;

void render_into(lv_obj_t* left);

// Shared step logic for the four nav callbacks below — dir is always ±1, so
// exactly one of a function's two bound-checks can ever trigger per call
// (g_view_month only ever leaves [0,11] by exactly 1 after a single step),
// same as when each direction had its own hand-written branch.
void nav_month(int dir) {
    g_view_month += dir;
    if (g_view_month < 0)  { g_view_month = 11; --g_view_year; }
    if (g_view_month > 11) { g_view_month = 0;  ++g_view_year; }
    if (g_left_panel) render_into(g_left_panel);
}

void nav_year(int dir) {
    g_view_year += dir;
    if (g_left_panel) render_into(g_left_panel);
}

void on_prev_month(lv_event_t*) { nav_month(-1); }
void on_next_month(lv_event_t*) { nav_month(+1); }
void on_prev_year(lv_event_t*)  { nav_year(-1); }
void on_next_year(lv_event_t*)  { nav_year(+1); }

// Shared square button shell for the header nav row (month/year chevrons).
// Sized generously and spaced apart by the caller (see `nav`'s pad_column)
// so adjacent buttons aren't an easy mis-tap target.
lv_obj_t* make_nav_btn(lv_obj_t* parent, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_size(btn, NAV_BTN_SIZE, NAV_BTN_SIZE);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(btn, theme::color(theme::COLOR_ELEV), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

lv_obj_t* make_chevron_line(lv_obj_t* btn, const lv_point_precise_t* pts) {
    lv_obj_t* chev = lv_line_create(btn);
    lv_line_set_points(chev, pts, 3);
    lv_obj_set_style_line_color(chev, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_line_width(chev, 4, 0);  // scaled with the +50% chevron glyphs
    lv_obj_set_style_line_rounded(chev, true, 0);
    lv_obj_clear_flag(chev, LV_OBJ_FLAG_CLICKABLE);
    return chev;
}

// Single chevron nav button — moves by one month. Matches the prototype's
// i-chev-left / i-chev-right. `pointing_left` selects the caret direction;
// both buttons share one of two static point sets (read-only — safe to
// reuse across instances, same convention as the OTA screen's checkmark
// tick in screen_ota_updating.cpp).
lv_obj_t* make_chevron_btn(lv_obj_t* parent, bool pointing_left, lv_event_cb_t cb) {
    lv_obj_t* btn = make_nav_btn(parent, cb);
    // Chevron glyph enlarged 50% (scaled about the 36px button's centre 18,18);
    // still fits within the button so the header height — and the day grid
    // below it — are unaffected.
    static lv_point_precise_t left_pts[3]  = {{23, 8}, {12, 18}, {23, 29}};
    static lv_point_precise_t right_pts[3] = {{12, 8}, {23, 18}, {12, 29}};
    make_chevron_line(btn, pointing_left ? left_pts : right_pts);
    return btn;
}

// Double-chevron nav button — moves by one year. Two chevrons drawn side by
// side inside the same square button.
lv_obj_t* make_double_chevron_btn(lv_obj_t* parent, bool pointing_left, lv_event_cb_t cb) {
    lv_obj_t* btn = make_nav_btn(parent, cb);
    // Enlarged 50% (scaled about centre 18,18), same as the single chevron.
    static lv_point_precise_t left_a[3]  = {{18, 8}, {8, 18}, {18, 29}};
    static lv_point_precise_t left_b[3]  = {{30, 8}, {20, 18}, {30, 29}};
    static lv_point_precise_t right_a[3] = {{6, 8}, {17, 18}, {6, 29}};
    static lv_point_precise_t right_b[3] = {{18, 8}, {29, 18}, {18, 29}};
    make_chevron_line(btn, pointing_left ? left_a : right_a);
    make_chevron_line(btn, pointing_left ? left_b : right_b);
    return btn;
}

// (Re)build the header + month grid inside `left`. Called on first entry
// and again on every prev/next month tap.
void render_into(lv_obj_t* left) {
    lv_obj_clean(left);

    // Normalize the viewed (year, month) through mktime/localtime_r — this
    // also lets on_prev_month/on_next_month freely push tm_mon outside
    // [0,11] and have the year roll over correctly.
    struct tm view_tm = {};
    view_tm.tm_year = g_view_year - 1900;
    view_tm.tm_mon  = g_view_month;
    view_tm.tm_mday = 1;
    view_tm.tm_hour = 12; // noon — sidesteps DST-boundary edge cases
    time_t view_tt = mktime(&view_tm);
    struct tm view;
    localtime_r(&view_tt, &view);
    g_view_year  = view.tm_year + 1900;
    g_view_month = view.tm_mon;

    char month_name[24];
    strftime(month_name, sizeof(month_name), "%B", &view);
    char header_buf[40];
    snprintf(header_buf, sizeof(header_buf), "%s %d", month_name, g_view_year);

    // First weekday of the month, shifted so Monday is column 0
    // (tm_wday is 0=Sun..6=Sat).
    int first_dow = (view.tm_wday + 6) % 7;

    // Days in the viewed month: day 0 of the following month.
    struct tm next_tm = {};
    next_tm.tm_year = g_view_year - 1900;
    next_tm.tm_mon  = g_view_month + 1;
    next_tm.tm_mday = 0;
    next_tm.tm_hour = 12;
    time_t next_tt = mktime(&next_tm);
    struct tm next_norm;
    localtime_r(&next_tt, &next_norm);
    int days_in_month = next_norm.tm_mday;

    // Days in the previous month (for the leading outside-month numbers).
    struct tm prev_tm = {};
    prev_tm.tm_year = g_view_year - 1900;
    prev_tm.tm_mon  = g_view_month;
    prev_tm.tm_mday = 0;
    prev_tm.tm_hour = 12;
    time_t prev_tt = mktime(&prev_tm);
    struct tm prev_norm;
    localtime_r(&prev_tt, &prev_norm);
    int days_in_prev_month = prev_norm.tm_mday;

    // Today, for the highlight.
    time_t now = time(nullptr);
    struct tm now_tm;
    localtime_r(&now, &now_tm);
    int today_year  = now_tm.tm_year + 1900;
    int today_month = now_tm.tm_mon;
    int today_day   = now_tm.tm_mday;

    if (today_year != g_cached_today_year || today_month != g_cached_today_month ||
        today_day != g_cached_today_day) {
        struct tm today_noon = {};
        today_noon.tm_year = today_year - 1900;
        today_noon.tm_mon  = today_month;
        today_noon.tm_mday = today_day;
        today_noon.tm_hour = 12;
        g_cached_today_tt     = mktime(&today_noon);
        g_cached_today_year   = today_year;
        g_cached_today_month  = today_month;
        g_cached_today_day    = today_day;
    }

    // ===== Header: "Month YYYY" + prev/next chevrons =====
    lv_obj_t* header = lv_obj_create(left);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    ui::clear_container(header);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_bottom(header, 12, 0);

    lv_obj_t* month_lbl = lv_label_create(header);
    lv_label_set_text(month_lbl, header_buf);
    lv_obj_set_style_text_font(month_lbl, theme::font_time(), 0);  // 30px
    lv_obj_set_style_text_color(month_lbl, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_translate_x(month_lbl, 20, 0);

    lv_obj_t* nav = lv_obj_create(header);
    lv_obj_set_size(nav, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ui::clear_container(nav);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Uniform gap between every chevron button (one flex pad_column applies the
    // same spacing to all four).
    constexpr int16_t CHEVRON_GAP = 35;  // -10% from 39
    lv_obj_set_style_pad_column(nav, CHEVRON_GAP, 0);

    // Order, left to right: year-back, month-back, month-forward, year-forward.
    make_double_chevron_btn(nav, /*pointing_left=*/true,  on_prev_year);
    make_chevron_btn(nav, /*pointing_left=*/true,  on_prev_month);
    make_chevron_btn(nav, /*pointing_left=*/false, on_next_month);
    make_double_chevron_btn(nav, /*pointing_left=*/false, on_next_year);

    // ===== Weekday label row — its own grid, separate from the day grid
    // below, with a small pad_bottom for the gap down to the date numbers. =====
    static lv_coord_t col_dsc[8] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
    };
    static lv_coord_t weekday_row_dsc[2] = { LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST };

    lv_obj_t* weekday_row = lv_obj_create(left);
    ui::clear_container(weekday_row);
    lv_obj_set_size(weekday_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_bottom(weekday_row, 4, 0);
    lv_obj_set_layout(weekday_row, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(weekday_row, col_dsc, weekday_row_dsc);

    static const char* WEEKDAYS[7] = { "M", "T", "W", "T", "F", "S", "S" };
    for (int c = 0; c < 7; ++c) {
        lv_obj_t* wd = lv_label_create(weekday_row);
        lv_label_set_text(wd, WEEKDAYS[c]);
        lv_obj_set_style_text_font(wd, theme::font_meta(), 0);
        lv_obj_set_style_text_color(wd, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
        lv_obj_clear_flag(wd, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_grid_cell(wd, LV_GRID_ALIGN_CENTER, c, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    }

    // ===== Day grid: N rows, 7 columns. Rows are FR(1) so they divide the
    // grid's height evenly; BADGE_SIZE is chosen so even a 6-week month keeps
    // each row >= the badge, otherwise the grid would clip the top/bottom rows. =====
    int total_cells = first_dow + days_in_month;
    int day_rows = (total_cells + 6) / 7; // round up to a full week

    // Max 6 day rows + 1 sentinel = 7.
    static lv_coord_t row_dsc[7];
    for (int i = 0; i < day_rows; ++i) row_dsc[i] = LV_GRID_FR(1);
    row_dsc[day_rows] = LV_GRID_TEMPLATE_LAST;

    lv_obj_t* grid = lv_obj_create(left);
    ui::clear_container(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

    int total_day_cells = day_rows * 7;

    // Locate today within the grid. Today shows not only when its own month is
    // viewed but also as a spillover (outside-month) day when paging one month
    // either side — e.g. June 30 appears in July's leading week. Compare by real
    // date so the year/month rollover of spillover days is handled correctly.
    struct tm cell0_tm = view;          // view = day 1 of the viewed month @ noon
    cell0_tm.tm_mday -= first_dow;      // back up to the grid's first (Monday) cell
    cell0_tm.tm_hour = 12;
    time_t cell0_tt = mktime(&cell0_tm);
    long day_index = (long)((g_cached_today_tt - cell0_tt + 43200) / 86400);  // +half day → nearest
    int  today_cell = (day_index >= 0 && day_index < total_day_cells) ? (int)day_index : -1;
    int  today_row  = (today_cell >= 0) ? today_cell / 7 : -1;
    // Bright tier when today's own month is on screen; fainter when today is
    // only a spillover day from the adjacent month being paged through.
    bool today_in_month = (g_view_year == today_year && g_view_month == today_month);

    // Faint-yellow band behind today's week. Added before the day badges so it
    // sits behind them; today's circle reads as the brighter marker on top.
    if (today_cell >= 0) {
        lv_obj_t* week_bg = lv_obj_create(grid);
        lv_obj_set_height(week_bg, BADGE_SIZE);
        lv_obj_set_style_radius(week_bg, BADGE_SIZE / 2, 0);
        lv_obj_set_style_bg_color(week_bg, theme::color(
            today_in_month ? theme::COLOR_ACCENT_FAINT : theme::COLOR_ACCENT_SOFT), 0);
        lv_obj_set_style_bg_opa(week_bg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(week_bg, 0, 0);
        lv_obj_set_style_pad_all(week_bg, 0, 0);
        lv_obj_clear_flag(week_bg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(week_bg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_grid_cell(week_bg, LV_GRID_ALIGN_STRETCH, 0, 7,
                                      LV_GRID_ALIGN_CENTER, today_row, 1);
    }
    for (int i = 0; i < total_day_cells; ++i) {
        int row = i / 7;
        int col = i % 7;
        int day_offset = i - first_dow;

        int day_num;
        bool outside;
        if (day_offset < 0) {
            day_num = days_in_prev_month + day_offset + 1;
            outside = true;
        } else if (day_offset >= days_in_month) {
            day_num = day_offset - days_in_month + 1;
            outside = true;
        } else {
            day_num = day_offset + 1;
            outside = false;
        }
        bool today = (i == today_cell);  // matches spillover days too (see today_cell)

        lv_obj_t* badge = lv_obj_create(grid);
        lv_obj_set_size(badge, BADGE_SIZE, BADGE_SIZE);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(badge, 0, 0);
        lv_obj_set_style_pad_all(badge, 0, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        if (today) {
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            // In-month today: darker-than-base accent gold behind a white number.
            // Spillover today (paged to an adjacent month): one tier fainter.
            lv_obj_set_style_bg_color(badge, theme::color(
                today_in_month ? theme::COLOR_ACCENT_LINE : theme::COLOR_ACCENT_FAINT), 0);
        } else {
            lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
        }
        lv_obj_set_grid_cell(badge, LV_GRID_ALIGN_CENTER, col, 1, LV_GRID_ALIGN_CENTER, row, 1);

        lv_obj_t* lbl = lv_label_create(badge);
        char buf[3];
        snprintf(buf, sizeof(buf), "%d", day_num);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, theme::font_title(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        if (today) {
            // Full-opacity number on the highlight (white in-month, near-white
            // when faint) — kept readable even though spillover days are dimmed.
            lv_obj_set_style_text_color(lbl,
                today_in_month ? lv_color_white() : theme::color(theme::COLOR_TEXT_PRIMARY), 0);
        } else if (outside) {
            lv_obj_set_style_text_color(lbl, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
            lv_obj_set_style_opa(lbl, LV_OPA_50, 0);
        } else {
            lv_obj_set_style_text_color(lbl, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
        }
        lv_obj_center(lbl);
    }
}

} // namespace

namespace screen_calendar {

void reset_view() {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    g_view_year  = tm_now.tm_year + 1900;
    g_view_month = tm_now.tm_mon;
}

void refresh_today() {
    // Re-render the currently-viewed month (navigation preserved); render_into()
    // re-reads the clock, so today + the current-week band recompute. No-op when
    // the calendar screen isn't up (g_left_panel is cleared on its delete).
    if (g_left_panel) render_into(g_left_panel);
}

lv_obj_t* create() {
    if (g_view_year < 0) reset_view(); // defensive — real entry path always resets first

    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    lv_obj_t* bar = widget_status_bar::create(screen);
    widget_status_bar::set_show_datetime(bar, false);

    lv_obj_t* body = ui::make_screen_body(screen);

    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, 528, lv_pct(100));
    ui::clear_container(left);
    lv_obj_set_style_pad_all(left, 16, 0);
    lv_obj_set_style_pad_row(left, 0, 0);  // no default inter-row gap; spacing is explicit below
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    g_left_panel = left;
    lv_obj_add_event_cb(screen, [](lv_event_t* e) {
        if (g_left_panel == (lv_obj_t*)lv_event_get_user_data(e)) g_left_panel = nullptr;
    }, LV_EVENT_DELETE, left);

    render_into(left);

    ui::make_panel_divider(body);
    widget_profile_card::create(body);
    return screen;
}

} // namespace screen_calendar
