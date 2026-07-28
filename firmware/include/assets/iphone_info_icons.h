#pragma once
#include <lvgl.h>

// Compiled-in raster icons for the iPhone Info/Stats overlay
// (modal_iphone_info.cpp) — missed calls, unread messages, notifications.
// 70x70 ARGB8888 (modal_iphone_info.cpp's ICON_BOX — drawn 1:1 with no LVGL
// scale transform; an earlier 28px-source + 2.5x-upscale attempt rendered
// visibly soft), same build pipeline as shortcut_icons.h
// (firmware/img/iphone_info_icons/convert_iphone_info_icons.py), separate
// prefix since these aren't media-mode shortcut assets.

namespace iphone_info_icons {

const lv_image_dsc_t* missed_call();
const lv_image_dsc_t* message();
const lv_image_dsc_t* bell();

} // namespace iphone_info_icons
