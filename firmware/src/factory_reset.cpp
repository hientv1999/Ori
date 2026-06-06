// Ori factory reset — shared execution path for local long-press and remote BLE.
//
// M5: wipes both NimBLE bond records + NVS bond addresses in addition to the
// "ori" namespace keys.

#include "factory_reset.h"

#include <Arduino.h>
#include "ori_log.h"

#include "nvs_store.h"
#include "photo_cache.h"
#include "ble/ble_manager.h"

namespace factory_reset {

void execute() {
    LOG("[factory_reset] execute — wiping NVS + BLE bonds + restart\n");

    // Wipe all NVS keys in the "ori" namespace.
    nvs::factory_reset();

    // Erase both cached photos from NVS and free their PSRAM decode buffers.
    photo_cache::clear();
    photo_cache::clear_pto();

    // Wipe both BLE bond records + NVS bond slot addresses.
    ble_manager::wipe_all_bonds();

    delay(200);
    ESP.restart();
}

} // namespace factory_reset
