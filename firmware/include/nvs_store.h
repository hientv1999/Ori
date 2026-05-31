#pragma once
#include <stdint.h>

// NVS scaffolding (Arduino Preferences). Single namespace "ori".
//
// Key layout:
//   "prov"  — bool: setup completed (first-boot flag)
//   "mode"  — uint8: 0=Calendar, 1=Media (immediate write, infrequent)
//
// Profile, photo, and meeting/PTO hashes are M5 scope — reserved slots.
namespace nvs {

void    init();
void    tick();  // reserved for future debounced writes; currently a no-op

// First-boot detection.
bool    is_first_boot();
void    mark_setup_complete();

// Mode toggle persistence (0 = Calendar, 1 = Media).
uint8_t get_mode();
void    set_mode(uint8_t mode);

// Factory reset — clears ALL keys in the "ori" namespace.
// Does NOT reboot; caller is responsible for calling ESP.restart() after.
void    factory_reset();

} // namespace nvs
