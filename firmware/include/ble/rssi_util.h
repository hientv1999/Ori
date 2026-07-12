#pragma once

#include <stdint.h>

namespace ble {

// Buckets a live RSSI reading (dBm, more negative = weaker) into the 0-4 bar
// scale used everywhere Ori or Orion render a signal-strength UI (iPhone
// Info overlay/modal, Orion's Ori Info modal). Thresholds are a standard-ish
// RSSI-to-bars ladder — BLE has no calibrated "signal strength" concept like
// cellular, so this is a reasonable approximation, not a spec'd value.
// Shared by ancs_client.cpp (iPhone link) and gatt_server.cpp (Orion link) —
// same ladder, same UI, one definition.
inline uint8_t rssi_to_bars(int rssi) {
    if (rssi >= -60) return 4;
    if (rssi >= -70) return 3;
    if (rssi >= -80) return 2;
    if (rssi >= -90) return 1;
    return 0;
}

} // namespace ble
