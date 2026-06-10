#pragma once

#include <lvgl.h>
#include <stdint.h>

// Ori — OTA-Updating full-screen takeover.
//
// Per ota.md: status bar hidden, profile card hidden, left panel hidden.
// Content: "Updating firmware… N%" + progress ring. All touch inert.
// Non-dismissable while a transfer is in flight.

namespace screen_ota_updating {

// Create the OTA screen and store it in the module state.
// The mock timer is NOT started — progress is driven by ota_receiver::poll()
// calling set_progress() on each PROGRESS frame.
lv_obj_t* create();

// Update the progress ring to the given percentage (0..100).
// Called directly from ota_receiver.cpp when a PROGRESS frame arrives.
// Thread-safe: safe to call from the Arduino main task (where ota_receiver
// runs) because lv_timer_handler() is also on the main task.
void set_progress(uint8_t pct);

// Switch the page from the live "Updating firmware / Downloading…" state to the
// "Installing firmware" state shown for the final frame before the panel goes
// dark for the flash commit. Called once at END after the image verifies.
// `linger_ms` is how long this frame stays up before the screen blanks — it
// drives the bottom countdown bar. Pass COMMIT_LINGER_MS from ota_receiver so the
// bar empties exactly as the commit begins. (Default matches it for the tester.)
void set_installing(uint32_t linger_ms = 3500);

// ── OTA flow screens (each returns a full-screen object) ────────────────────
// The button callbacks are supplied by the caller so the same screens serve
// both the real OTA flow (ota_receiver) and the debug serial tester.

// Post-reboot acknowledgement — shows the running version + a checkmark.
// Tertiary "Close" button → on_close (clears the NVS "needs ack" flag).
lv_obj_t* create_updated_ack(const char* version, lv_event_cb_t on_close);

// "Update failed" — `message` is the plain-language reason. Tertiary "Close"
// button → on_close (dismiss back to runtime).
lv_obj_t* create_error(const char* message, lv_event_cb_t on_close);

} // namespace screen_ota_updating
