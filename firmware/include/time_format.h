#pragma once
#include <stddef.h>
#include <stdint.h>

// 12-/24-hour clock display preference.
//
// Owns the single source of truth for how wall-clock times are rendered across
// the whole UI (status bar, digital + analog clock faces, meeting list, ANCS
// notification timestamps). The setting is persisted to NVS and configured by
// Orion over BLE via the Device Settings characteristic (key "h", ble-protocol.md
// §3/§4/§6.4). 24-hour is the default and preserves the original behaviour.
//
// Meeting times are stored internally as canonical 24-hour "HH:MM" strings and
// are parsed back for alert/expiry logic — never reformat those in place; use
// reformat() only at render time.
namespace time_format {

// Load the persisted setting into the RAM cache. Call once at boot, after
// nvs::init(). Until called, is_24h() returns true (24-hour).
void init();

// 0 = 24-hour (default), 1 = 12-hour. Matches the NVS + BLE wire value.
uint8_t get();
bool    is_24h();

// Update + persist to NVS. Any non-zero value maps to 12-hour. Safe to call
// from any non-LVGL-timer context (BLE event dispatch, debug cycler).
void set(uint8_t fmt);

// Format a wall-clock time into out: "14:30" (24h) or "2:30 PM" (12h).
// hour24 is 0-23. Always null-terminated (given sz > 0).
void hhmm(char* out, size_t sz, int hour24, int min);

// Same time value, but split so a caller can render the "AM"/"PM" suffix as
// its own (typically smaller/secondary) subtext instead of appending it to
// the hour:minute string. out_time gets "14:30" (24h) or "2:30" (12h,
// no suffix); out_suffix gets "AM"/"PM" for 12h or an empty string for 24h.
// hour24 is 0-23. Both buffers are always null-terminated (given sz > 0).
void hhmm_split(char* out_time, size_t time_sz, char* out_suffix, size_t suffix_sz,
                 int hour24, int min);

// Reformat a canonical 24-hour "HH:MM" string into the configured format.
// Empty/invalid input yields an empty string.
void reformat(const char* hhmm24, char* out, size_t sz);

} // namespace time_format
