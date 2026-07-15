#pragma once
#include <stdint.h>
#include <stddef.h>

// NVS scaffolding (Arduino Preferences). Single namespace "ori".
//
// Key layout:
//   "prov"        — bool: setup completed (first-boot flag)
//   "mode"        — uint8: 0=Calendar, 1=Media (immediate write, infrequent)
//   "clock_face"  — uint8: 0=Digital, 1=Analog (immediate write, infrequent)
//   "time_fmt"    — uint8: 0=24-hour, 1=12-hour (immediate write, infrequent)
//   "notif_filt"  — uint8: ANCS filter level 0-3
//   "sc_1/2/3"    — string: shortcut slot token (≤19 chars + null)
//   "seek_step"   — uint8: double-tap seek step, seconds (1-60, default 10)
//
// Profile, photo, and meeting/Time Off hashes live in the "ori" namespace too,
// under keys owned by nvs_sync.h.
namespace nvs {

void    init();
void    tick();  // reserved for future debounced writes; currently a no-op

// First-boot detection.
bool    is_first_boot();
void    mark_setup_complete();

// Setup resume bookmarks.  Both are only meaningful while is_first_boot() is
// true, and are cleared when setup fully completes or on factory reset.
//
//   mark_orion_bonded()  — set when the Orion bond forms (Step 2), before the
//     first sync.  A power cycle mid-Orioning resumes on the Link-Orion screen
//     (is_awaiting_sync()) instead of Welcome; Orion reconnects via the stored
//     bond and re-drives the sync.
//   mark_orion_synced()  — set after the first sync completes; supersedes the
//     sync bookmark so a power cycle resumes at the iPhone pairing step
//     (is_awaiting_phone_pairing()).
//
// Resume priority (latest step wins): phone-pairing > sync > Welcome.
void    mark_orion_bonded();
bool    is_awaiting_sync();
void    mark_orion_synced();
bool    is_awaiting_phone_pairing();

// Reverts mark_orion_bonded() — called when the link drops after a bond was
// confirmed (SyncControl{BEGIN} received) but before the first sync ever
// reached SyncEnd. Without this, a connection dropped in that exact window
// (e.g. the PC's Bluetooth toggled off, or the Orion app killed, mid-sync)
// leaves the "awaiting first sync" bookmark set forever with no automatic
// way to clear it — see ble_manager.cpp's OrionDisconnected handler, which
// pairs this with un-persisting the bond address itself so Ori resumes
// SETUP advertising instead of staying stuck as RUNTIME/bonded.
void    clear_orion_bonded();

// Mode toggle persistence (0 = Calendar, 1 = Media).
uint8_t get_mode();
void    set_mode(uint8_t mode);

// Clock-face preference (0 = Digital, 1 = Analog). Drives which of
// screen_clock / screen_clock_analog the Clock state shows.
uint8_t get_clock_face();
void    set_clock_face(uint8_t face);

// Time-format preference (0 = 24-hour, 1 = 12-hour). Default 24-hour. Set by
// Orion via Device Settings (char 000E, key "h"); persisted so it survives
// power cycles. Drives every wall-clock display via the time_format module.
uint8_t get_time_format();
void    set_time_format(uint8_t fmt);

// ANCS notification filter level (0=Disabled, 1=CallOnly, 2=Important, 3=All).
// Default 3 (All). Set by Orion via Device Settings (char 000E); persisted so the setting
// survives power cycles without Orion needing to resend it on every reconnect.
uint8_t get_notif_filter();
void    set_notif_filter(uint8_t level);

// Double-tap seek step, in seconds (1-60). Default 10. Set by Orion via
// Device Settings (char 000E, key "k"); persisted so it survives power
// cycles. See media-mode.md — double-tap left/right third of the album art
// seeks backward/forward by this many seconds.
uint8_t get_seek_step_s();
void    set_seek_step_s(uint8_t seconds);

// Shortcut slot tokens — NVS-persisted (≤19 chars each) so Orion can read them
// back on (re)connect via the Device Settings characteristic. get_shortcut_slots()
// fills each buffer up to slot_sz-1 chars (null-terminated). Defaults on a fresh
// device: "vol-mute" / "mic-mute" / "screenshot".
void get_shortcut_slots(char* s1, char* s2, char* s3, size_t slot_sz);
void set_shortcut_slots(const char* s1, const char* s2, const char* s3);

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
