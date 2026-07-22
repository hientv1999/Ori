#pragma once

#include <stdint.h>

#include "widgets/widget_profile_card.h"

// Ori — Weather Alert / Low Battery Alert overlays (state-machine.md's
// "Weather Alert / Low Battery Alert overlays" section).
//
// Two one-shot, dismiss-only "notice" overlays (accent-tinted — NOT the
// danger-red factory-reset/unpair styling) that Ori arms and fires entirely
// on its own, from data it already holds (live weather condition, the
// bonded phone's live battery %) plus six small config values Orion pushes
// over the existing Device Settings characteristic (ble-protocol.md §4/§6.4)
// — no new BLE characteristic and no "show this now" push from Orion.
//
// Structural precedent: modal_incoming_call.cpp — a one-shot overlay built
// directly on lv_screen_active(), with no AppState entry and no
// state-machine priority registration. This module owns ONLY the overlay
// itself; the trigger-eval logic (when to call these — day-rollover
// latch reset, battery-recovery latch clear, the actual threshold/offset
// math) lives in state_machine.cpp's existing 1 s tick, the same split
// modal_incoming_call.cpp keeps from ancs_client.cpp's call-state logic
// that decides when to invoke show()/notify_active().

namespace modal_device_alert {

// Raise the Weather Alert overlay. `condition` must be Rain, Thunderstorm,
// or Snow (the only three state_machine.cpp ever calls this for).
// `end_time_str` is the user's Working Hours end time, already formatted per
// the 12h/24h time_format preference (e.g. "17:30" or "5:30 PM") —
// time_format::hhmm(), not reimplemented here.
void show_weather_alert(widget_profile_card::WeatherCondition condition,
                         const char* end_time_str);

// Raise the Low Battery Alert overlay. `phone_kind_word` is "iPhone"/"iPad"
// (ancs_client::phone_kind_word()); `phone_name` is the bonded device's GAP
// name (ancs_client::phone_name()); `battery_pct` is 0-100
// (ancs_client::phone_stats().battery).
void show_low_battery_alert(const char* phone_kind_word, const char* phone_name,
                             uint8_t battery_pct);

} // namespace modal_device_alert
