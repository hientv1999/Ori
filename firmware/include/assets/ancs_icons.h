#pragma once
#include <stdint.h>
#include <lvgl.h>

namespace ancs_icons {

// Returns the brand fill colour (0xRRGGBB) for the given icon token.
// Unknown or null tokens return 0x555555 (neutral grey).
uint32_t color(const char* token);

// Returns the compiled-in raster image descriptor for a token, or nullptr
// if no image asset exists for that token (fallback: colored circle).
const lv_image_dsc_t* image(const char* token);

} // namespace ancs_icons
