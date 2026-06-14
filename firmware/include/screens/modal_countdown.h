#pragma once

#include <lvgl.h>

// Ori — 5-minute pre-meeting countdown modal.
//
// Centered scrim + circular progress ring + three single-line (ellipsised)
// text rows: meeting title, organizer, location — then a Close button.
// organizer / location may be nullptr or "" (the row is omitted when empty).
// Dismissed via the Close button only.
// seconds_remaining: time until meeting start in seconds (≤ 300).

namespace modal_countdown {

lv_obj_t* create(lv_obj_t* base_screen,
                 const char* meeting_title,
                 const char* organizer,
                 const char* location,
                 int seconds_remaining);

} // namespace modal_countdown
