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

// Post-OTA acknowledgement flag. Set just before the reboot at the end of a
// successful USB CDC OTA; read on the next boot to show the "Firmware updated"
// ack screen, which persists across reboots until the user taps Close.
//   set:   stores the new firmware version string ("needs ack" = present)
//   get:   true + fills `buf` if an unacknowledged update is pending
//   clear: removes the flag (user acknowledged)
void    set_ota_ack(const char* version);
bool    get_ota_ack(char* buf, uint32_t len);
void    clear_ota_ack();

} // namespace nvs
