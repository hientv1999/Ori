#pragma once

#include <lvgl.h>

// Small corner-badge glyphs used inside modal_ancs_list.cpp's per-row icon
// badge — e.g. the silent/muted indicator. Separate from iphone_info_icons.h
// (that modal's own full-size 70x70 stat icons) and from ancs_icons.h (the
// 60x60 per-app/category icons the row itself shows) — these are baked much
// smaller (18x18, convert_ancs_badge_icons.py's SIZE) to sit inside a 26px
// corner badge without any LVGL scale transform.

namespace ancs_badge_icons {

// Bell-off glyph — user's own bell.png (iphone_info_icons/) with a diagonal
// slash cut into it, same "slash cut into the glyph itself" technique the
// status-bar phone icon uses for its disconnected state (screen-layout.md).
const lv_image_dsc_t* silent();

} // namespace ancs_badge_icons
