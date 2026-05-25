#pragma once

// LVGL's lv_conf.h sets LV_TICK_CUSTOM_INCLUDE = "Arduino.h". On the
// embedded build that resolves to the real Arduino headers. On the desktop
// simulator we don't have those — this shim redirects to our minimal
// arduino_shim.h which provides just the symbols LVGL actually touches
// (`millis()`).
//
// This header also provides a minimal Serial stub so that screen/*.cpp files
// that call Serial.println() for debug logging compile cleanly in the sim.
// The stub is a no-op — log output on desktop goes to std::printf instead.

#include "arduino_shim.h"

#ifdef __cplusplus
#include <cstdio>

// Minimal Serial stub — only the methods actually called by firmware screen
// files are implemented. All are no-ops or redirect to stdout.
struct _SerialStub {
    template <typename T>
    _SerialStub& print(T)   { return *this; }
    template <typename T>
    _SerialStub& println(T) { return *this; }
    _SerialStub& println()  { return *this; }
    template <typename T, typename B>
    _SerialStub& print(T v, B)   { return *this; }
    template <typename T, typename B>
    _SerialStub& println(T v, B) { return *this; }
};

inline _SerialStub Serial;
#endif // __cplusplus
