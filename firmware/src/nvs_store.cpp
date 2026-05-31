#include "nvs_store.h"

#include <Arduino.h>
#include <Preferences.h>

namespace {

constexpr const char* NAMESPACE = "ori";

// Key layout (see nvs_store.h for canonical table).
constexpr const char* k_provisioned = "prov";  // bool: setup completed
constexpr const char* k_mode        = "mode";  // uint8: 0=Calendar 1=Controls

Preferences prefs;

} // namespace

namespace nvs {

void init() {
    // Open read-write so the namespace is created if it doesn't exist yet
    // (fresh flash, post-factory-reset).
    if (!prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        Serial.println("[nvs] ERROR: could not open namespace — NVS partition missing?");
        return;
    }
    prefs.end();
    Serial.println("[nvs] ready");
}

void tick() {
    // Reserved for future debounced writes.
}

// ── First-boot detection ───────────────────────────────────────────────────

bool is_first_boot() {
    bool provisioned = false;
    if (prefs.begin(NAMESPACE, /*readOnly=*/true)) {
        provisioned = prefs.getBool(k_provisioned, false);
        prefs.end();
    }
    return !provisioned;
}

void mark_setup_complete() {
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putBool(k_provisioned, true);
        prefs.end();
    }
    Serial.println("[nvs] setup complete — provisioned flag set");
}

// ── Mode toggle persistence ────────────────────────────────────────────────

uint8_t get_mode() {
    uint8_t v = 0;  // default: Calendar
    if (prefs.begin(NAMESPACE, /*readOnly=*/true)) {
        v = (uint8_t)prefs.getUChar(k_mode, 0);
        prefs.end();
    }
    return v;
}

void set_mode(uint8_t mode) {
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.putUChar(k_mode, mode);
        prefs.end();
    }
    Serial.printf("[nvs] mode=%d\n", (int)mode);
}

// ── Factory reset ──────────────────────────────────────────────────────────

void factory_reset() {
    if (prefs.begin(NAMESPACE, /*readOnly=*/false)) {
        prefs.clear();
        prefs.end();
    }
    Serial.println("[nvs] factory reset wipe complete");
}

} // namespace nvs
