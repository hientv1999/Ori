#include "assets/iphone_info_icons.h"

// Compiled-in raster icons for the iPhone Info/Stats overlay (28x28
// ARGB8888). Add an extern here for each new icon added to
// firmware/src/assets/iphoneinfo_{cname}.c
extern "C" {
    extern const lv_image_dsc_t missed_call;
    extern const lv_image_dsc_t message;
    extern const lv_image_dsc_t bell;
}

namespace iphone_info_icons {

const lv_image_dsc_t* missed_call() { return &::missed_call; }
const lv_image_dsc_t* message()     { return &::message; }
const lv_image_dsc_t* bell()        { return &::bell; }

} // namespace iphone_info_icons
