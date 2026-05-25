#pragma once

#include <lvgl.h>

// Ori — OTA-Updating full-screen takeover.
//
// Per ota.md: status bar hidden, profile card hidden, left panel hidden.
// Content: "Updating firmware… N%" + progress ring. All touch inert.
// Non-dismissable while a transfer is in flight.
//
// M3 ticks a mock 0 → 100% over ~10 s and loops, so we can eyeball the
// animation budget. The real progress source is M5 (OTA Control PROGRESS
// notifications driven by Arduino's Update library).

namespace screen_ota_updating {

lv_obj_t* create();

} // namespace screen_ota_updating
