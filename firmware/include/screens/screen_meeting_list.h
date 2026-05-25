#pragma once

#include <lvgl.h>

#include "mock_data.h"

// Ori — left-panel meeting list screen (work hours, with cached meetings).
//
// Layout per prototype:
//   .meeting-list  — vertical scrollable container, visible scrollbar.
//   .meeting       — grid: [108 px time] [1fr content], wraps internally.
//   .meeting.overlap — left accent stripe + accent-color times.
//   .synced-pill   — "SYNCED · 12 min ago", top-right when cached.
//
// The screen composes: status bar (top), left panel + divider + right
// profile card (below). All standard widgets — no fancy LVGL gymnastics.

namespace screen_meeting_list {

// `cached` adds the "SYNCED · X min ago" pill at the top-right of the list.
lv_obj_t* create(mock_data::MeetingList list, bool cached);

} // namespace screen_meeting_list
