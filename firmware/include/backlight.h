#pragma once
#include <stdint.h>

// Backlight control for the Waveshare ESP32-S3 4.3" panel.
//
// The backlight is **binary on/off** — the LED driver is gated by CH422G EXIO2
// (digital-only). PWM dimming is not possible on this hardware without a
// hardware modification. See gestures.md and memory.md for the rationale.
//
// Bidirectional: the two-finger swipe gesture on Ori and the ON/OFF toggle in
// the Orion app both call set_on(). init() restores the saved state from NVS
// BEFORE the panel comes up so the user never sees a flash on boot.
//
// Important: turning the backlight OFF does NOT put the device to sleep.
// The ESP32 stays fully running — BLE, timers, and state machine continue.
// Only the LED is gated.
namespace backlight {

void init();
void set_on(bool on);
bool is_on();

} // namespace backlight
