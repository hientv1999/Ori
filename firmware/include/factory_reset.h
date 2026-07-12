#pragma once

// Ori factory reset helper — shared by both the local long-press path
// (modal_factory_reset) and the remote BLE path (gatt_server Factory
// Reset Command char 0x0008).
//
// Both paths converge here so the exact same NVS-wipe + bond-wipe +
// restart sequence runs regardless of trigger source.

namespace factory_reset {

// Wipe NVS (all "ori" namespace keys), wipe both BLE bonds, delay 200 ms,
// then call ESP.restart(). Does not return.
//
// Important: do NOT call this directly from an LVGL event callback — the
// LCD DMA ISR can fire during the NVS sector erase and trigger a cache
// exception. Always call via a deferred lv_timer or from the main loop
// after the callback returns. The LVGL modal_factory_reset.cpp Reset button
// already defers via a one-shot lv_timer; the BLE path uses the ble_manager
// event queue.
void execute();

} // namespace factory_reset
