#pragma once

// Ori — Minimal BLE manager (M5 debug: advertise-only).
//
// THREADING RULE:
//   NimBLE runs its own FreeRTOS task. LVGL is NOT thread-safe.
//   NimBLE callbacks MUST only write to volatile state / flags.
//   All LVGL calls happen exclusively inside ble_core::poll().

namespace ble_core {

// Initialise NimBLE, derive the device name (Ori-XX-XX from MAC), and
// start advertising. Call once from setup() BEFORE lcd_panel::init() so
// the NimBLE flash writes (BLE config, IRK) complete before the RGB DMA
// starts running — the DMA ISR is in flash and faults if the cache is
// toggled underneath it.
void init();

// Process pending BLE events in the LVGL context.
// Call from loop() BEFORE lv_timer_handler(). Currently a no-op in the
// advertise-only build; wired up when GATT is added back.
void poll();

// Returns the "Ori-XX-XX" device name derived at init() time.
// Returns "" if init() has not been called yet.
const char* device_name();

} // namespace ble_core
