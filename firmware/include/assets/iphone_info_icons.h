#pragma once
#include <lvgl.h>

// Compiled-in raster icons for the iPhone Info/Stats overlay
// (modal_iphone_info.cpp) — missed calls, unread messages, notifications.
// 28x28 ARGB8888, same build pipeline as shortcut_icons.h
// (firmware/img/iphone_info_icons/convert_iphone_info_icons.py), separate
// prefix since these aren't media-mode shortcut assets.

namespace iphone_info_icons {

const lv_image_dsc_t* missed_call();
const lv_image_dsc_t* message();
const lv_image_dsc_t* bell();

} // namespace iphone_info_icons
