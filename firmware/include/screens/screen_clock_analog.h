#pragma once

#include <lvgl.h>

// Ori — analog clock screen, alternate face for the Clock state.
//
// Same entry/exit and status-bar behaviour as screen_clock.h (digital face):
// status bar present with date/time hidden, profile card on the right.
// Which face is shown is the user's clock-face preference
// (nvs::get_clock_face() / state_machine::set_clock_face()).

namespace screen_clock_analog {

lv_obj_t* create();

} // namespace screen_clock_analog
