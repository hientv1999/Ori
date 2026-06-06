#include "assets/shortcut_icons.h"

#include <string.h>

// Compiled-in raster shortcut icon assets (40×40 ARGB8888).
// Add an extern here and a row in k_images for each new icon added to
// firmware/src/assets/shortcut_{cname}.c
extern "C" {
    extern const lv_image_dsc_t vol_mute;
    extern const lv_image_dsc_t mic_mute;
    extern const lv_image_dsc_t screenshot;
    extern const lv_image_dsc_t lock_screen;
    extern const lv_image_dsc_t favorite;
    extern const lv_image_dsc_t calculator;
}

namespace shortcut_icons {

namespace {
struct Entry { const char* token; const lv_image_dsc_t* dsc; };
static const Entry k_images[] = {
    { "vol-mute",   &vol_mute   },
    { "mic-mute",   &mic_mute   },
    { "screenshot", &screenshot },
    { "lock-screen",&lock_screen},
    { "favorite",    &favorite    },
    { "calculator",  &calculator  },
};
} // namespace

const lv_image_dsc_t* image(const char* token) {
    if (!token) return nullptr;
    for (const auto& e : k_images) {
        if (strcmp(e.token, token) == 0) return e.dsc;
    }
    return nullptr;
}

} // namespace shortcut_icons
