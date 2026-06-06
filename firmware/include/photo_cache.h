#pragma once

#include <stdint.h>
#include <stddef.h>
#include <lvgl.h>

// Ori — profile photo decode-once PSRAM cache.
//
// The profile photo arrives as a JPEG over BLE (228×228, ≤40 KB).
// This module persists the raw JPEG to NVS ("photo" namespace) and decodes
// it once to an RGB565 pixel buffer in PSRAM.  LVGL renders directly from
// the PSRAM buffer via a static lv_image_dsc_t — zero re-decode per frame.
//
// Thread-safety: all public functions are called from the Arduino main task
// (either during setup or via the BLE event queue dispatch in ble_manager).
// No locking is required.

namespace photo_cache {

// Called from main setup() after nvs::init().
// Loads the JPEG from NVS (if present) and decodes it to PSRAM so the
// profile card shows the stored photo immediately on boot, before BLE
// reconnects.
void init();

// Called when the profile photo BLE transfer completes (from ble_manager
// poll, on the Arduino main task).  Saves the JPEG to NVS, decodes to
// PSRAM, and calls widget_profile_card::set_photo() on the active screen.
//
// The caller transfers ownership of the buffer to photo_cache: do NOT free
// buf after this call.  photo_cache::store() frees it internally after
// copying to NVS.
void store(uint8_t* jpeg, size_t len);

// Returns the decoded profile photo descriptor, or nullptr if none stored.
const lv_image_dsc_t* get();

// Erases profile photo from NVS and frees PSRAM buffer.
void clear();

// ── PTO destination image ─────────────────────────────────────────────────
// Same decode-once pattern. len == 0 means the user set no destination image
// in Orion — store_pto(jpeg, 0) clears the cache and get_pto() returns nullptr.

void init_pto();
void store_pto(uint8_t* jpeg, size_t len);  // takes ownership; frees jpeg internally
const lv_image_dsc_t* get_pto();
void clear_pto();

} // namespace photo_cache
