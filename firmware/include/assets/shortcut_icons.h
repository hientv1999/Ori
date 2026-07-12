#pragma once
#include <lvgl.h>

namespace shortcut_icons {

// Returns the compiled-in raster image descriptor for a shortcut icon token,
// or nullptr if no asset exists for that token (fallback: placeholder letter).
// Supported tokens: "vol-mute", "mic-mute", "screenshot", "lock-screen", "favorite-1", "favorite-2", "favorite-3", "calculator", "copy", "cut", "paste", "redo", "save", "undo"
const lv_image_dsc_t* image(const char* token);

} // namespace shortcut_icons
