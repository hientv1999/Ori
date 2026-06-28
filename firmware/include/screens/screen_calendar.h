#pragma once

typedef struct _lv_obj_t lv_obj_t;

// Ori — Calendar (month view) screen. Entered by long-pressing the
// status-bar time (short tap still enters screen_clock). View-only month
// grid with today highlighted; left/right chevrons in the header navigate
// months. Mode-toggle (calendar-return glyph) returns to the previous
// mode, mirroring screen_clock.h.
namespace screen_calendar {

lv_obj_t* create();

// Reset the navigated-to month back to the current month. Call before
// every fresh entry (state_machine::on_calendar_enter()) so re-opening the
// calendar always starts on today's month rather than wherever a previous
// session's navigation left off.
void reset_view();

} // namespace screen_calendar
