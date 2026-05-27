// Ori — Minimal BLE manager (M5 debug: advertise-only).
//
// Goal: confirm NimBLE can initialise and advertise while the LCD RGB DMA
// and LVGL are running simultaneously, without crashing.
//
// Boot-order constraint:
//   NimBLEDevice::init() writes BLE config + IRK to NVS flash.  Flash writes
//   temporarily disable the CPU cache.  The LCD RGB DMA ISR lives in flash
//   (not IRAM), so it faults if the cache is toggled while the DMA is running.
//   Fix: call ble_core::init() in setup() BEFORE lcd_panel::init().

#include "ble/ble_core.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEAdvertising.h>
#include <NimBLEAddress.h>

// ── Device name ─────────────────────────────────────────────────────────────
static char g_device_name[16] = {};

// ── Advertising ──────────────────────────────────────────────────────────────
static void start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    adv->setName(g_device_name);
    // Include Ori Sync Service UUID so Orion can scan-filter by service.
    adv->addServiceUUID(NimBLEUUID("6F726900-0000-4F72-9F00-000000000000"));
    // 100 ms interval for setup/unbound state.
    adv->setMinInterval(160);  // 160 * 0.625 ms = 100 ms
    adv->setMaxInterval(168);
    adv->start();
    Serial.printf("[ble] advertising as \"%s\"\n", g_device_name);
}

namespace ble_core {

void init() {
    // Init NimBLE with a blank name first to get the BLE address.
    NimBLEDevice::init("Ori");
    start_advertising();
    Serial.println("[ble] init complete (advertise-only build)");
}

void poll() {
    // No-op in advertise-only build.
    // GATT event handling will be added back once stable.
}

const char* device_name() { return g_device_name; }

} // namespace ble_core
