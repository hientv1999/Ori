#pragma once

// ── Ori firmware logging ───────────────────────────────────────────────────────
//
// Toggle all firmware log output with a single flag.
// To silence everything: set ORI_LOG_ENABLED=0 in platformio.ini build_flags
// or define it before including this header.
//
// Usage — identical to printf (caller is responsible for \n):
//   LOG("[tag] value=%u\n", val);
//   LOG("[tag] message\n");

#ifndef ORI_LOG_ENABLED
  #define ORI_LOG_ENABLED 1
#endif

#if ORI_LOG_ENABLED
  #include <Arduino.h>
  #define LOG(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
  #define LOG(fmt, ...) ((void)0)
#endif
