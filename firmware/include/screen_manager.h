#pragma once

// Ori — screen lifecycle owner and serial debug cycler (M3).
//
// The serial cycler lets you navigate every screen from the serial monitor
// without touching the device. Define ORI_DEBUG_SERIAL in platformio.ini
// build_flags to enable it, then press '?' for the key map.
//
// State machine + persistence are M4.

namespace screen_manager {

// Load the initial screen.
void init();

// Call from loop() every pass.
// When ORI_DEBUG_SERIAL is defined, drains the serial port and dispatches
// debug key commands. No-op in production builds.
void poll_serial();

} // namespace screen_manager
