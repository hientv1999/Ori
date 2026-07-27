#include "factory_info.h"

#include <Arduino.h>
#include "ori_log.h"
#include <Preferences.h>

namespace {

constexpr const char* PARTITION = "factory";
constexpr const char* NAMESPACE = "factory";
constexpr const char* k_serial  = "sn";   // string: serial number
constexpr const char* k_mfg     = "mfg";  // string: manufacture date, "YYYY-MM-DD"

// Serial is a fixed 12 ASCII digits (provisioning.md's DDMMYYNNNNCC); the cap
// keeps generous headroom rather than sizing to exactly 13, so a mis-provisioned
// over-long value is truncated and then rejected by the 12-digit check in
// identify_responder.cpp rather than overflowing. mfg date is a fixed-width
// "YYYY-MM-DD" (10 chars).
char g_serial[32] = "";
char g_mfg[16]    = "";

} // namespace

namespace factory_info {

void init() {
    Preferences prefs;
    // readOnly — this module never writes; a missing/unprovisioned partition
    // (dev boards flashed without running the factory provisioning step)
    // just means begin() fails or the keys are absent, both handled by the
    // getString() defaults below.
    if (!prefs.begin(NAMESPACE, /*readOnly=*/true, PARTITION)) {
        LOG("[factory_info] no factory partition data (unprovisioned dev unit)\n");
        return;
    }
    prefs.getString(k_serial, g_serial, sizeof(g_serial));
    prefs.getString(k_mfg, g_mfg, sizeof(g_mfg));
    prefs.end();
    LOG("[factory_info] serial=%s mfg_date=%s\n",
        g_serial[0] ? g_serial : "(none)", g_mfg[0] ? g_mfg : "(none)");
}

const char* serial_number()    { return g_serial; }
const char* manufacture_date() { return g_mfg; }

} // namespace factory_info
