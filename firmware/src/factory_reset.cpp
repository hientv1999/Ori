// Ori factory reset — shared execution path for local long-press.

#include "factory_reset.h"

#include <Arduino.h>

#include "nvs_store.h"

namespace factory_reset {

void execute() {
    Serial.println("[factory_reset] execute — wiping NVS + restart");

    // Wipe all NVS keys in the "ori" namespace.
    nvs::factory_reset();

    // BLE bond removal will be added when M5 (BLE) is re-introduced.

    delay(200);
    ESP.restart();
}

} // namespace factory_reset
