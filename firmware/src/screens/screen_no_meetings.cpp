#include "screens/screen_no_meetings.h"

#include <lvgl.h>
#include <time.h>

#include "app_state.h"
#include "holiday_data.h"
#include "state_machine.h"
#include "theme.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// "No meetings today" empty state.
//
//   .empty {
//     centered, glyph 297x234 (COLOR_TEXT_TERTIARY + today's cell in accent),
//     headline 30px COLOR_TEXT_PRIMARY,
//     sub 18px COLOR_TEXT_TERTIARY
//   }
//
// LVGL has no native calendar glyph and no symbol font is enabled, so the icon
// is composed from primitives (see make_cal_glyph). Real icon font lands in M8.

namespace {

// out_holiday_name is set to today's holiday name, or nullptr if today isn't
// one (or the clock isn't set yet, so "today" isn't known at all) — lets
// build_left() swap the subtitle without re-deriving today's date itself.
lv_obj_t* make_cal_glyph(lv_obj_t* parent, const char** out_holiday_name) {
    // Calendar glyph, drawn from LVGL primitives (no symbol font available).
    // 297 wide; the empty band above the binding tabs is trimmed and the frame
    // height is cropped to the calendar body's bottom edge (234) so there's no
    // empty band between the glyph and the headline below — the headline's
    // pad_top is then the true visible gap. The inner grid lays out the ACTUAL
    // current month: 7 day columns, the real first-weekday offset of the 1st,
    // and today's cell filled in accent gold. When the clock isn't set
    // yet (cold boot before any time source), it falls back to a generic full
    // grid so the icon still reads as a calendar without asserting a wrong date.
    //   1. Hollow rounded body rect
    //   2. Full-width header divider bar near the top
    //   3. Two top binding tabs straddling the top edge
    //   4. Real current-month day-cell grid below the header

    // Small positioned box helper — filled or outlined, non-interactive.
    auto box = [&](lv_obj_t* par, int16_t x, int16_t y, int16_t w, int16_t h,
                   uint32_t col, int16_t radius, bool filled) {
        lv_obj_t* o = lv_obj_create(par);
        lv_obj_set_size(o, w, h);
        lv_obj_set_pos(o, x, y);
        if (filled) {
            lv_obj_set_style_bg_color(o, theme::color(col), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(o, 0, 0);
        } else {
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(o, theme::color(col), 0);
            lv_obj_set_style_border_width(o, 5, 0);
        }
        lv_obj_set_style_radius(o, radius, 0);
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        return o;
    };

    // Outer frame — 297 wide, height cropped to the body's bottom (234).
    lv_obj_t* glyph = lv_obj_create(parent);
    lv_obj_set_size(glyph, 297, 234);
    ui::clear_container(glyph);

    const uint32_t line = theme::COLOR_TEXT_TERTIARY;

    // 1) Body — hollow rounded rect.
    box(glyph, 37, 43, 221, 191, line, 20, /*filled=*/false);
    // 2) Header divider — full-width bar (below the month title; top block −20%).
    box(glyph, 37, 100, 221, 10, line, 0, /*filled=*/true);
    // 3) Two top binding tabs straddling the top edge (clips, 20% shorter).
    box(glyph, 90, 19, 17, 42, line, 7, /*filled=*/true);
    box(glyph, 190, 19, 17, 42, line, 7, /*filled=*/true);

    // 4) Day-cell grid — the real current month, laid out Monday-first.
    constexpr int16_t CW = 12, CH = 8;                // cell size (+20%)
    constexpr int16_t PX = 29, PY = 19;               // cell pitch (+10% with the symbol)
    constexpr int16_t FIRST_X = 54;                   // x of column 0 (re-centred)
    constexpr int16_t REGION_Y = 114, REGION_H = 112; // grid band: below header, inside body

    int first_dow = 0;       // Monday-first weekday of the 1st (0..6)
    int days_in_month = 35;  // cold-boot fallback: a full 5-row block
    int today_day = 0;       // 0 = no highlight (fallback)
    char title_buf[24] = {0};// "Month YYYY" — stays empty when the clock isn't set
    *out_holiday_name = nullptr;

    if (app_state::clock_is_set()) {
        time_t now = time(nullptr);
        struct tm now_tm;
        localtime_r(&now, &now_tm);
        today_day = now_tm.tm_mday;
        strftime(title_buf, sizeof(title_buf), "%b %Y", &now_tm);  // abbreviated → fits at 30px
        // tm_mon is 0-based; holiday_data::name_for() takes a 1-based month.
        const holiday_data::Info* holiday = holiday_data::name_for(
            holiday_data::country(), holiday_data::region(), now_tm.tm_year + 1900, now_tm.tm_mon + 1, today_day);
        *out_holiday_name = holiday ? holiday->name : nullptr;

        // Weekday of the 1st of this month (Monday = column 0).
        struct tm first_tm = now_tm;
        first_tm.tm_mday = 1;
        first_tm.tm_hour = 12;  // noon — sidesteps DST-boundary edge cases
        time_t first_tt = mktime(&first_tm);
        struct tm first_norm;
        localtime_r(&first_tt, &first_norm);
        first_dow = (first_norm.tm_wday + 6) % 7;

        // Days in month = "day 0" of next month, normalized.
        struct tm next_tm = now_tm;
        next_tm.tm_mon += 1;
        next_tm.tm_mday = 0;
        next_tm.tm_hour = 12;
        time_t next_tt = mktime(&next_tm);
        struct tm next_norm;
        localtime_r(&next_tt, &next_norm);
        days_in_month = next_norm.tm_mday;
    }

    int total_cells = first_dow + days_in_month;
    int day_rows = (total_cells + 6) / 7;            // round up to whole weeks
    if (day_rows < 1) day_rows = 1;
    int block_h = (day_rows - 1) * PY + CH;
    int start_y = REGION_Y + (REGION_H - block_h) / 2;  // vertically centre the weeks

    // Current week (the row today sits in) gets a faint-yellow tint; today's
    // own cell stays full accent gold; all other days use the muted line colour.
    int today_row = (today_day > 0) ? (first_dow + today_day - 1) / 7 : -1;
    for (int day = 1; day <= days_in_month; ++day) {
        int idx = first_dow + (day - 1);
        int row = idx / 7;
        int16_t x = FIRST_X + (idx % 7) * PX;
        int16_t y = start_y + row * PY;
        // Today's cell swaps to holiday-red in place of accent gold when
        // today is a holiday — at this cell's tiny (12x8) scale a hollow ring
        // like the month view's wouldn't read clearly, so this uses the same
        // solid-swap treatment as the "no meetings" screen's headline/
        // subtitle below (COLOR_DANGER, matching --holiday's prototype value).
        uint32_t cell_col = (day == today_day)
                          ? (*out_holiday_name ? theme::COLOR_DANGER : theme::COLOR_ACCENT)
                          : (row == today_row) ? theme::COLOR_ACCENT_FAINT
                                               : line;
        box(glyph, x, y, CW, CH, cell_col, 2, /*filled=*/true);
    }

    // Month + year title, centred in the strip between the binding tabs and the
    // header divider bar. Omitted when the clock isn't set (no real date).
    if (title_buf[0]) {
        lv_obj_t* title = lv_label_create(glyph);
        lv_label_set_text(title, title_buf);
        lv_obj_set_style_text_font(title, theme::font_time(), 0);  // 30px (+50%)
        lv_obj_set_style_text_color(title, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_width(title, 221);                              // body width
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(title, 37, 62);  // body x; strip below the tabs, above the divider
        lv_obj_clear_flag(title, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    }

    return glyph;
}

} // namespace

namespace screen_no_meetings {

lv_obj_t* build_left(lv_obj_t* body) {
    // Left panel — centered glyph + headline + subtitle.
    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, 528, lv_pct(100));
    ui::clear_container(left);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char* holiday_name = nullptr;
    lv_obj_t* glyph = make_cal_glyph(left, &holiday_name);
    lv_obj_set_style_pad_bottom(glyph, 0, 0);
    // Tapping the calendar symbol opens the month (Calendar) view — same entry
    // point as long-pressing the status-bar time. The glyph's child shapes are
    // non-clickable, so the tap lands on this container.
    lv_obj_add_flag(glyph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(glyph, LV_OPA_60, LV_STATE_PRESSED);  // press feedback
    lv_obj_add_event_cb(glyph, [](lv_event_t*) {
        state_machine::on_calendar_enter();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* headline = lv_label_create(left);
    lv_label_set_text_static(headline, "No meetings today");
    lv_obj_set_style_text_color(headline, theme::color(theme::COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(headline, theme::font_display(), 0);
    lv_obj_set_style_pad_top(headline, 9, 0);  // tightened 60% (was 22)

    // Names the holiday instead of the generic line when today is one —
    // color stays the same (COLOR_TEXT_TERTIARY); only the glyph's today-cell
    // color change (above) and this text carry the signal, matching the
    // prototype's .empty .sub treatment (content changes, color doesn't).
    lv_obj_t* sub = lv_label_create(left);
    lv_label_set_text(sub, holiday_name ? holiday_name : "Enjoy the focus time");
    lv_obj_set_style_text_color(sub, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_font(sub, theme::font_title(), 0);  // 26px (font_meta 24 + 2)

    return left;
}

lv_obj_t* create() {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    widget_status_bar::create(screen);

    lv_obj_t* body = ui::make_screen_body(screen);

    lv_obj_t* left = build_left(body);

    ui::make_panel_divider(body);
    widget_profile_card::create(body);

    // Register with the state machine so a reconnect sync can refresh only
    // this left panel in place instead of rebuilding the whole screen.
    state_machine::register_runtime_calendar(screen, body, left);

    return screen;
}

} // namespace screen_no_meetings
