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

} // namespace screen_ota_updating
