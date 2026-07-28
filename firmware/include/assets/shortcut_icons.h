#pragma once
#include <lvgl.h>

namespace shortcut_icons {

// Returns the compiled-in raster image descriptor for a shortcut icon token,
// or nullptr if no asset exists for that token — the caller (screen_media_mode
// ::apply_shortcut_icon()) hides that slot's button entirely rather than
// showing a placeholder, per media-mode.md's "Unrecognized token -> hide the
// slot" rule.
// Supported tokens: "vol-mute", "mic-mute", "screenshot", "lock-screen", "favorite-1", "favorite-2", "favorite-3", "calculator", "copy", "cut", "paste", "redo", "save", "undo"
const lv_image_dsc_t* image(const char* token);

} // namespace shortcut_icons
