#include "assets/ancs_badge_icons.h"

// Compiled-in raster icon for modal_ancs_list.cpp's small corner badge
// (18x18 ARGB8888). Add an extern here for each new icon added to
// firmware/src/assets/ancsbadge_{cname}.c
extern "C" {
    extern const lv_image_dsc_t silent;
}

namespace ancs_badge_icons {

const lv_image_dsc_t* silent() { return &::silent; }

} // namespace ancs_badge_icons
