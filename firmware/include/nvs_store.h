#pragma once
#include <stdint.h>

// NVS scaffolding (Arduino Preferences). Single namespace "ori".
//
// Backlight state writes are debounced ~2 s — call nvs::tick() every loop
// pass to let the deadline fire. No FreeRTOS tasks, no timers, no callbacks.
//
// Key layout:
//   "bl"    — bool: backlight on/off (owned by backlight.cpp, debounced)
//   "prov"  — bool: setup completed (first-boot flag)
//   "mode"  — uint8: 0=Calendar, 1=Controls (immediate write, infrequent)
//
// Profile, photo, and meeting/PTO hashes are M5 scope — reserved slots.
namespace nvs {

void    init();
void    tick();

// Backlight (owned by backlight.cpp — do not call from elsewhere).
bool    load_backlight_on(bool fallback);
void    save_backlight_on(bool on);   // debounced ~2 s

// First-boot detection.
// is_first_boot() returns true when the "prov" key is missing or false.
bool    is_first_boot();
void    mark_setup_complete();      // sets "prov" = true

// Mode toggle persistence (0 = Calendar, 1 = Controls).
// Written immediately on every toggle (infrequent; no debounce needed).
uint8_t get_mode();
void    set_mode(uint8_t mode);

// Factory reset — clears ALL keys in the "ori" namespace.
// Does NOT reboot; caller is responsible for calling ESP.restart() after.
void    factory_reset();

} // namespace nvs
