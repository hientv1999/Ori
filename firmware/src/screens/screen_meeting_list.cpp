#include "screens/screen_meeting_list.h"

#include <lvgl.h>
#include <cstring>

#include "app_state.h"
#include "state_machine.h"
#include "theme.h"
#include "time_format.h"
#include "ui_helpers.h"
#include "widgets/widget_profile_card.h"
#include "widgets/widget_status_bar.h"

// Meeting list screen layout (matches prototype 1:1 in panel space):
//
//   ┌ 800 ────────────────────────────────────────────────────────┐
//   │ status bar 84                                                │
//   ├────────────────────┬─┬─────────────────────────────────────┤
//   │ left panel 528     │3│ profile card 269                     │
//   │                    │ │                                       │
//   └────────────────────┴─┴───────────────────────────────────────┘
//
// The left panel hosts a vertically scrollable list of meeting rows, each:
//
//   .meeting {
//     grid: [108 px time] [1fr content]
//     padding: 14 px / 4 px
//     border-bottom: 1 px COLOR_DIVIDER
//   }
//
// Overlap accent: when `meeting.overlap` is true, both the time text and
// the title turn COLOR_ACCENT (matches the prototype's behaviour exactly).
// In-progress red wins over overlap gold — "you should be in this room right
// now" is more actionable than "this overlaps with another item".

namespace {

// Color priority shared by every meeting visual (detail modal, row time,
// row title): in_progress (danger red) > overlap (accent gold) > normal.
// An in-progress meeting that also overlaps reads as in_progress — "you
// should be in this room right now" is more actionable than "this overlaps".
uint32_t meeting_state_color(const app_state::Meeting& m, uint32_t normal_color) {
    if (m.in_progress) return theme::COLOR_DANGER;
    if (m.overlap)     return theme::COLOR_ACCENT;
    return normal_color;
}

constexpr int16_t LEFT_PANEL_WIDTH  = 528;
constexpr int16_t TIME_COL_WIDTH    = 108;  // fits "HH:MM" at font_h2 (24-hour)
constexpr int16_t TIME_COL_WIDTH_12 = 132;  // fits "12:30 PM" at font_h2 (12-hour)
constexpr int16_t ROW_GAP_X         = 16;
constexpr int16_t ROW_PAD_Y         = 14;

// Full-screen meeting detail modal — dismissed via the Close button only.
static void show_meeting_detail(lv_obj_t* screen, const app_state::Meeting& m) {
    // Full-screen scrim — absorbs taps behind the dialog; no click-to-dismiss.
    lv_obj_t* scrim = ui::make_scrim(screen);

    // Outer box — full screen, non-scrollable flex column. Not clickable so
    // taps on empty space fall through to the scrim. OVERFLOW_VISIBLE lets the
    // Close button glow bleed past the bottom edge.
    lv_obj_t* box = lv_obj_create(scrim);
    lv_obj_set_size(box, 800, 480);
    lv_obj_set_pos(box, 0, 0);
    ui::clear_container(box);
    lv_obj_set_style_pad_left(box, 40, 0);
    lv_obj_set_style_pad_right(box, 40, 0);
    lv_obj_set_style_pad_top(box, 24, 0);
    lv_obj_set_style_pad_bottom(box, 24, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Scrollable text area — grows to fill all space above the Close button.
    lv_obj_t* scroll_area = lv_obj_create(box);
    lv_obj_set_width(scroll_area, lv_pct(100));
    lv_obj_set_flex_grow(scroll_area, 1);
    lv_obj_set_style_bg_opa(scroll_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_area, 0, 0);
    lv_obj_set_style_pad_all(scroll_area, 0, 0);
    lv_obj_add_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(scroll_area, LV_DIR_VER);
    lv_obj_set_flex_flow(scroll_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Top spacer — centres content when it is shorter than the scroll area.
    ui::make_flex_spacer(scroll_area);

    const uint32_t state_col = meeting_state_color(m, theme::COLOR_TEXT_PRIMARY);

    // Title — full text, wrapping, state-colored.
    lv_obj_t* title = lv_label_create(scroll_area);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT); // single-line with ellipsis
    lv_label_set_text(title, m.title);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, theme::font_title(), 0);
    lv_obj_set_style_text_color(title, theme::color(state_col), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    // Location — full text, wrapping.
    lv_obj_t* loc = lv_label_create(scroll_area);
    lv_label_set_long_mode(loc, LV_LABEL_LONG_DOT);
    lv_label_set_text(loc, m.loc);
    lv_obj_set_width(loc, lv_pct(100));
    lv_obj_set_style_text_font(loc, theme::font_meta(), 0);
    lv_obj_set_style_text_color(loc, theme::color(theme::COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_align(loc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(loc, 16, 0);

    // Organizer — single line with ellipsis. Without an explicit width the
    // label auto-sizes to its text and a long name runs off the screen edge;
    // the 1-line height makes LONG_DOT clip instead of wrapping (LVGL 9).
    lv_obj_t* org = lv_label_create(scroll_area);
    lv_label_set_long_mode(org, LV_LABEL_LONG_DOT);
    lv_obj_set_width(org, lv_pct(100));
    lv_obj_set_height(org, theme::font_meta()->line_height);
    lv_label_set_text(org, m.org);
    lv_obj_set_style_text_font(org, theme::font_meta(), 0);
    lv_obj_set_style_text_color(org, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_text_align(org, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(org, 4, 0);

    // Time range — state-colored, same as title. En-dash separator (matches the
    // prototype's detail overlay and the Time Off date range).
    char start_disp[16], end_disp[16];
    time_format::reformat(m.start, start_disp, sizeof(start_disp));
    time_format::reformat(m.end,   end_disp,   sizeof(end_disp));
    char time_buf[48];
    lv_snprintf(time_buf, sizeof(time_buf), "%s \xe2\x80\x93 %s", start_disp, end_disp); // UTF-8 en-dash
    lv_obj_t* time_lbl = lv_label_create(scroll_area);
    lv_label_set_text(time_lbl, time_buf);
    lv_obj_set_style_text_font(time_lbl, theme::font_h2(), 0);
    lv_obj_set_style_text_color(time_lbl, theme::color(state_col), 0);
    lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(time_lbl, 12, 0);

    // Bottom spacer — mirrors top spacer.
    ui::make_flex_spacer(scroll_area);

    // Close button — direct child of box, pinned at the bottom of the screen.
    lv_obj_t* close_btn = ui::make_btn(box, "Close", ui::BtnStyle::Tertiary,
                                       nullptr, nullptr, 12, 26, theme::font_meta());
    lv_obj_add_event_cb(close_btn, ui::close_scrim_cb, LV_EVENT_CLICKED, scrim);
}

lv_obj_t* make_meeting_row(lv_obj_t* parent, const app_state::Meeting& m) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, theme::color(theme::COLOR_DIVIDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 3, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, ROW_PAD_Y, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, ROW_PAD_Y, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, ROW_GAP_X, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    // Row is tappable — opens the detail modal.
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, theme::color(theme::COLOR_ELEV), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

    // LVGL keeps the pointer (no copy), so col_dsc must persist — hence static.
    // Every row in a build uses the same format, so updating [0] in place is safe.
    static lv_coord_t col_dsc[] = { TIME_COL_WIDTH, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    col_dsc[0] = time_format::is_24h() ? TIME_COL_WIDTH : TIME_COL_WIDTH_12;
    static lv_coord_t row_dsc[] = { LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST };
    lv_obj_set_grid_dsc_array(row, col_dsc, row_dsc);
    lv_obj_set_layout(row, LV_LAYOUT_GRID);

    // === Time block (column 0) ===
    lv_obj_t* time_block = lv_obj_create(row);
    lv_obj_set_size(time_block, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ui::clear_container(time_block);
    lv_obj_set_style_pad_top(time_block, 2, 0);
    lv_obj_set_flex_flow(time_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(time_block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_grid_cell(time_block, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_START, 0, 1);

    const uint32_t time_primary_color   = meeting_state_color(m, theme::COLOR_TEXT_PRIMARY);
    const uint32_t time_secondary_color = meeting_state_color(m, theme::COLOR_TEXT_TERTIARY);

    // Meeting times are stored canonical 24-hour ("HH:MM"); reformat for display.
    char start_disp[16], end_disp[16];
    time_format::reformat(m.start, start_disp, sizeof(start_disp));
    time_format::reformat(m.end,   end_disp,   sizeof(end_disp));

    lv_obj_t* start = lv_label_create(time_block);
    lv_label_set_text(start, start_disp);
    lv_obj_set_style_text_font(start, theme::font_h2(), 0);
    lv_obj_set_style_text_color(start, theme::color(time_primary_color), 0);

    lv_obj_t* end = lv_label_create(time_block);
    lv_label_set_text(end, end_disp);
    lv_obj_set_style_text_font(end, theme::font_meta(), 0);
    lv_obj_set_style_text_color(end, theme::color(time_secondary_color), 0);
    lv_obj_set_style_pad_top(end, 2, 0);
    if (m.overlap || m.in_progress) lv_obj_set_style_opa(end, LV_OPA_70, 0);

    // === Content block (column 1) ===
    lv_obj_t* content = lv_obj_create(row);
    lv_obj_set_height(content, LV_SIZE_CONTENT); // width owned by grid STRETCH — do not set LV_SIZE_CONTENT here or lv_pct() children can't resolve against it in LVGL 9
    ui::clear_container(content);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_grid_cell(content, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, 0, 1);

    // Title — single-line with ellipsis. In-progress red wins over overlap gold.
    lv_obj_t* title = lv_label_create(content);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_height(title, theme::font_title()->line_height); // LVGL 9: LV_SIZE_CONTENT height lets text wrap freely; explicit 1-line height makes LONG_DOT clip instead
    lv_label_set_text(title, m.title);
    lv_obj_set_style_text_font(title, theme::font_title(), 0);
    lv_obj_set_style_text_color(title, theme::color(meeting_state_color(m, theme::COLOR_TEXT_PRIMARY)), 0);

    // Meta row: location · organizer.
    lv_obj_t* meta = lv_obj_create(content);
    lv_obj_set_size(meta, lv_pct(100), LV_SIZE_CONTENT);
    ui::clear_container(meta);
    lv_obj_set_style_pad_top(meta, 6, 0);
    lv_obj_set_style_pad_column(meta, 12, 0);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* loc = lv_label_create(meta);
    lv_label_set_long_mode(loc, LV_LABEL_LONG_DOT);
    lv_obj_set_width(loc, 220);
    lv_obj_set_height(loc, theme::font_meta()->line_height);
    lv_label_set_text(loc, m.loc);
    lv_obj_set_style_text_font(loc, theme::font_meta(), 0);
    lv_obj_set_style_text_color(loc, theme::color(theme::COLOR_TEXT_SECONDARY), 0);

    lv_obj_t* dot = lv_obj_create(meta);
    lv_obj_set_size(dot, 4, 4);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* org = lv_label_create(meta);
    lv_label_set_long_mode(org, LV_LABEL_LONG_DOT);
    lv_obj_set_width(org, 140);
    lv_obj_set_height(org, theme::font_meta()->line_height);
    lv_label_set_text(org, m.org);
    lv_obj_set_style_text_font(org, theme::font_meta(), 0);
    lv_obj_set_style_text_color(org, theme::color(theme::COLOR_TEXT_SECONDARY), 0);

    // Row click → meeting detail modal, unless the meeting starts within the
    // 5-minute countdown window, in which case the countdown screen (same one
    // the automatic pre-meeting alert uses) takes over instead.
    lv_obj_add_event_cb(row, [](lv_event_t* e) {
        const auto* mp = static_cast<const app_state::Meeting*>(lv_event_get_user_data(e));
        if (state_machine::show_countdown_if_imminent(*mp)) return;
        show_meeting_detail(lv_obj_get_screen((lv_obj_t*)lv_event_get_target(e)), *mp);
    }, LV_EVENT_CLICKED, (void*)&m);

    return row;
}

lv_obj_t* make_synced_pill(lv_obj_t* parent) {
    lv_obj_t* pill = lv_obj_create(parent);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, theme::color(0x1A1D22), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 100, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_left(pill, 10, 0);
    lv_obj_set_style_pad_right(pill, 10, 0);
    lv_obj_set_style_pad_top(pill, 4, 0);
    lv_obj_set_style_pad_bottom(pill, 4, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* label = lv_label_create(pill);
    lv_label_set_text(label, app_state::synced_pill_text());
    lv_obj_set_style_text_font(label, theme::font_meta(), 0);
    lv_obj_set_style_text_color(label, theme::color(theme::COLOR_TEXT_TERTIARY), 0);
    lv_obj_center(label);

    // Text has minute-granularity ("LAST SYNCED · N min ago") but the timer
    // ticks every second — cache the last string so 59 out of 60 ticks skip
    // the redundant lv_label_set_text()/re-layout.
    struct PillCtx {
        lv_obj_t* label;
        char      last_text[48];
    };
    auto* ctx = new PillCtx{label, {}};
    strncpy(ctx->last_text, app_state::synced_pill_text(), sizeof(ctx->last_text) - 1);

    // Self-contained 1-second timer. Cleaned up by LV_EVENT_DELETE — no global
    // label pointer needed, so deferred screen deletion can never clear a
    // newer screen's state.
    lv_timer_t* pill_timer = lv_timer_create([](lv_timer_t* t) {
        auto* ctx = (PillCtx*)lv_timer_get_user_data(t);
        const char* text = app_state::synced_pill_text();
        if (strcmp(text, ctx->last_text) == 0) return;
        strncpy(ctx->last_text, text, sizeof(ctx->last_text) - 1);
        ctx->last_text[sizeof(ctx->last_text) - 1] = '\0';
        lv_label_set_text(ctx->label, text);
    }, 1000, (void*)ctx);

    lv_obj_add_event_cb(label, [](lv_event_t* e) {
        lv_timer_t* t = (lv_timer_t*)lv_event_get_user_data(e);
        delete (PillCtx*)lv_timer_get_user_data(t);
        lv_timer_delete(t);
    }, LV_EVENT_DELETE, (void*)pill_timer);

    return pill;
}

} // namespace

namespace screen_meeting_list {

lv_obj_t* build_left(lv_obj_t* body, app_state::MeetingList list, bool cached) {
    // Left panel.
    lv_obj_t* left = lv_obj_create(body);
    lv_obj_set_size(left, LEFT_PANEL_WIDTH, lv_pct(100));
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    // Scrollable list inside the left panel.
    lv_obj_t* list_obj = lv_obj_create(left);
    lv_obj_set_size(list_obj, LEFT_PANEL_WIDTH, lv_pct(100));
    lv_obj_set_pos(list_obj, 0, 0);
    lv_obj_set_style_bg_opa(list_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_obj, 0, 0);
    lv_obj_set_style_pad_top(list_obj, 12, 0);
    lv_obj_set_style_pad_bottom(list_obj, 14, 0);
    // 8 px here + each row's own 4 px pad_left (make_meeting_row) = 12 px from
    // the screen edge — matches the status bar's PAD_X (widget_status_bar.cpp),
    // so a meeting row's time text lines up vertically with the status-bar
    // hour:minute text above it.
    lv_obj_set_style_pad_left(list_obj, 8, 0);
    lv_obj_set_style_pad_right(list_obj, 18, 0);
    lv_obj_set_flex_flow(list_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(list_obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_obj, LV_SCROLLBAR_MODE_ON);
    lv_obj_clear_flag(list_obj, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    // Visible scrollbar styled to match the prototype's right-edge thumb.
    lv_obj_set_style_bg_color(list_obj, theme::color(theme::COLOR_TEXT_TERTIARY), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list_obj, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list_obj, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list_obj, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(list_obj, 2, LV_PART_SCROLLBAR);

    for (size_t i = 0; i < list.count; ++i) {
        make_meeting_row(list_obj, list.items[i]);
    }

    // Synced pill (cached state) overlays the top-right of the left panel, after meeting rows.
    if (cached) {
        lv_obj_t* pill = make_synced_pill(left);
        lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -18, 8);
        lv_obj_move_foreground(pill);
    }

    return left;
}

lv_obj_t* create(app_state::MeetingList list, bool cached) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    theme::apply_to_screen(screen);

    // Status bar across the top.
    lv_obj_t* bar = widget_status_bar::create(screen);
    lv_obj_set_pos(bar, 0, 0);

    lv_obj_t* body = ui::make_screen_body(screen);

    lv_obj_t* left = build_left(body, list, cached);

    ui::make_panel_divider(body);

    // Right profile card.
    widget_profile_card::create(body);

    // Register with the state machine so a reconnect sync can refresh only
    // this left panel in place instead of rebuilding the whole screen.
    state_machine::register_runtime_calendar(screen, body, left);

    return screen;
}

} // namespace screen_meeting_list
