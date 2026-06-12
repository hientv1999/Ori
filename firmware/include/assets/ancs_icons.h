#pragma once
#include <stdint.h>
#include <lvgl.h>

namespace ancs_icons {

// Returns the brand fill colour (0xRRGGBB) for the given icon token.
// Unknown or null tokens return 0x555555 (neutral grey).
uint32_t color(const char* token);

// Returns the compiled-in raster image descriptor for a known app token, or
// nullptr if the token has no brand asset (e.g. "unknown"). Callers should fall
// back to category_image() for unmapped apps.
const lv_image_dsc_t* image(const char* token);

// Fallback icon for an app with no brand asset, chosen by ANCS CategoryID
// (1=IncomingCall … 11=Entertainment). Unknown/Other categories return the
// generic notification (bell) icon. Never returns nullptr.
const lv_image_dsc_t* category_image(uint8_t category);

} // namespace ancs_icons
