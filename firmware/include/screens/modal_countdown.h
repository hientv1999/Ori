#pragma once

#include <lvgl.h>

// Ori — 5-minute pre-meeting countdown modal.
//
// Centered scrim + circular progress ring + meeting title + "Starts at HH:MM"
// subtitle. Tap anywhere on the modal to dismiss.
// seconds_remaining: time until meeting start in seconds (≤ 300).

namespace modal_countdown {

lv_obj_t* create(lv_obj_t* base_screen,
                 const char* meeting_title,
                 const char* when_text,
                 int seconds_remaining);

} // namespace modal_countdown
