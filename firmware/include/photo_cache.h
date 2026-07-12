#pragma once

#include <stdint.h>
#include <stddef.h>
#include <lvgl.h>

// Ori — profile photo decode-once PSRAM cache.
//
// The profile photo arrives as a JPEG over BLE (228×228, ≤40 KB).
// This module persists the raw JPEG to LittleFS (/photos/) and decodes
// it once to an RGB565 pixel buffer in PSRAM.  LVGL renders directly from
// the PSRAM buffer via a static lv_image_dsc_t — zero re-decode per frame.
//
// Thread-safety: all public functions are called from the Arduino main task
// (either during setup or via the BLE event queue dispatch in ble_manager).
// No locking is required.

namespace photo_cache {

// Mount LittleFS. Call once from setup() before any other photo_cache function.
void mount_fs();

// Called from main setup() after nvs::init().
// Loads the JPEG from LittleFS (if present) and decodes it to PSRAM so the
// profile card shows the stored photo immediately on boot, before BLE
// reconnects.
void init();

// Called when the profile photo BLE transfer completes (from ble_manager
// poll, on the Arduino main task).  Saves the JPEG to LittleFS, decodes to
// PSRAM, and calls widget_profile_card::set_photo() on the active screen.
//
// The caller transfers ownership of the buffer to photo_cache: do NOT free
// buf after this call.  photo_cache::store() frees it internally after
// copying to LittleFS.
void store(uint8_t* jpeg, size_t len);

// Returns the decoded profile photo descriptor, or nullptr if none stored.
const lv_image_dsc_t* get();

// Erases profile photo from LittleFS and frees PSRAM buffer.
void clear();

// Decode a compiled-in placeholder JPEG (228×228) into PSRAM.
// Call once at boot with the raw bytes from profile_placeholder.c.
// get_profile_placeholder() returns nullptr until called successfully.
void init_profile_placeholder(const uint8_t* jpeg, size_t len);
const lv_image_dsc_t* get_profile_placeholder();

// ── Time Off destination image ────────────────────────────────────────────
// Same decode-once pattern. len == 0 means the user set no destination image
// in Orion — store_time_off(jpeg, 0) clears the cache and get_time_off() returns nullptr.

void init_time_off();
void store_time_off(uint8_t* jpeg, size_t len);  // takes ownership; frees jpeg internally
const lv_image_dsc_t* get_time_off();
void clear_time_off();

// Decode a compiled-in JPEG (from flash) into PSRAM as the Time Off placeholder.
// Call once at boot with the raw bytes from time_off_placeholder.c.
// get_time_off_placeholder() returns nullptr until this is called successfully.
void init_time_off_placeholder(const uint8_t* jpeg, size_t len);
const lv_image_dsc_t* get_time_off_placeholder();

// ── Shared decode utility ─────────────────────────────────────────────────
// Decode a raw JPEG into a freshly PSRAM-allocated RGB565 buffer.
// Caller must heap_caps_free() the returned pointer when done.
// Does NOT take ownership of jpeg. Returns nullptr on failure.
uint16_t* decode_to_psram(const uint8_t* jpeg, size_t len, uint16_t w, uint16_t h);

// Fill an lv_image_dsc_t from a decoded PSRAM RGB565 buffer.
void fill_image_dsc(lv_image_dsc_t* dsc, const uint16_t* buf, uint16_t w, uint16_t h);

} // namespace photo_cache
