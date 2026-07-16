#include "ble/iphone_model_map.h"

#include <string.h>

namespace iphone_model {

struct Entry {
    const char* id;
    const char* name;
};

// Naming convention: the plain marketing name (e.g. "iPhone 4") when its
// identifier is the ONLY one that ever resolves to it. The moment two or
// more identifiers would otherwise collapse to the identical string, each
// gets a short " — <connectivity>" suffix, further parenthesized by radio
// type (GSM/CDMA) or region (China/US/Global) only where THAT'S what
// actually distinguishes the siblings — e.g. iPad2,1..iPad2,4 (all "iPad 2")
// become "iPad 2 — Wi-Fi", "iPad 2 — Wi-Fi + 3G (GSM)", "iPad 2 — Wi-Fi + 3G
// (CDMA)", "iPad 2 — Wi-Fi" (a later respin, identical to iPad2,1 in every
// way that matters here). Two siblings that are genuinely indistinguishable
// at this granularity (e.g. iPad8,1/iPad8,2 — same Wi-Fi-only variant, differ
// only by storage/RAM tier) intentionally still resolve to the same string.
// Kept short by g_phone_device_type's 64-byte budget (ancs_client.cpp) —
// longest current entries land around 59-62 bytes; see that file's comment.
static const Entry k_models[] = {
    {"iPhone1,1",  "iPhone"},
    {"iPhone1,2",  "iPhone 3G"},
    {"iPhone2,1",  "iPhone 3GS"},
    {"iPhone3,1",  "iPhone 4 (GSM)"},
    {"iPhone3,2",  "iPhone 4 (GSM)"},
    {"iPhone3,3",  "iPhone 4 (CDMA)"},
    {"iPhone4,1",  "iPhone 4S"},
    {"iPhone5,1",  "iPhone 5 (GSM)"},
    {"iPhone5,2",  "iPhone 5 (CDMA)"},
    {"iPhone5,3",  "iPhone 5C (CDMA)"},
    {"iPhone5,4",  "iPhone 5C (GSM)"},
    {"iPhone6,1",  "iPhone 5S (CDMA)"},
    {"iPhone6,2",  "iPhone 5S (GSM)"},
    {"iPhone7,1",  "iPhone 6 Plus"},
    {"iPhone7,2",  "iPhone 6"},
    {"iPhone8,1",  "iPhone 6s"},
    {"iPhone8,2",  "iPhone 6s Plus"},
    {"iPhone8,4",  "iPhone SE"},
    {"iPhone9,1",  "iPhone 7 (CDMA)"},
    {"iPhone9,2",  "iPhone 7 Plus (CDMA)"},
    {"iPhone9,3",  "iPhone 7 (GSM)"},
    {"iPhone9,4",  "iPhone 7 Plus (GSM)"},
    {"iPhone10,1", "iPhone 8 (CDMA)"},
    {"iPhone10,2", "iPhone 8 Plus (CDMA)"},
    {"iPhone10,3", "iPhone X (CDMA)"},
    {"iPhone10,4", "iPhone 8 (GSM)"},
    {"iPhone10,5", "iPhone 8 Plus (GSM)"},
    {"iPhone10,6", "iPhone X (GSM)"},
    {"iPhone11,2", "iPhone XS"},
    {"iPhone11,4", "iPhone XS Max (China)"},
    {"iPhone11,6", "iPhone XS Max"},
    {"iPhone11,8", "iPhone XR"},
    {"iPhone12,1", "iPhone 11"},
    {"iPhone12,3", "iPhone 11 Pro"},
    {"iPhone12,5", "iPhone 11 Pro Max"},
    {"iPhone12,8", "iPhone SE (2nd gen)"},
    {"iPhone13,1", "iPhone 12 mini"},
    {"iPhone13,2", "iPhone 12"},
    {"iPhone13,3", "iPhone 12 Pro"},
    {"iPhone13,4", "iPhone 12 Pro Max"},
    {"iPhone14,2", "iPhone 13 Pro"},
    {"iPhone14,3", "iPhone 13 Pro Max"},
    {"iPhone14,4", "iPhone 13 mini"},
    {"iPhone14,5", "iPhone 13"},
    {"iPhone14,6", "iPhone SE (3rd gen)"},
    {"iPhone14,7", "iPhone 14"},
    {"iPhone14,8", "iPhone 14 Plus"},
    {"iPhone15,2", "iPhone 14 Pro"},
    {"iPhone15,3", "iPhone 14 Pro Max"},
    {"iPhone15,4", "iPhone 15"},
    {"iPhone15,5", "iPhone 15 Plus"},
    {"iPhone16,1", "iPhone 15 Pro"},
    {"iPhone16,2", "iPhone 15 Pro Max"},
    {"iPhone17,1", "iPhone 16 Pro"},
    {"iPhone17,2", "iPhone 16 Pro Max"},
    {"iPhone17,3", "iPhone 16"},
    {"iPhone17,4", "iPhone 16 Plus"},
    {"iPhone17,5", "iPhone 16e"},
    {"iPhone18,1", "iPhone 17 Pro"},
    {"iPhone18,2", "iPhone 17 Pro Max"},
    {"iPhone18,3", "iPhone 17"},
    {"iPhone18,4", "iPhone Air"},
    {"iPhone18,5", "iPhone 17e"},

    // ── iPad — base line ─────────────────────────────────────────────────
    {"iPad1,1",   "iPad (1st gen)"},
    {"iPad2,1",   "iPad 2 — Wi-Fi"},
    {"iPad2,2",   "iPad 2 — Wi-Fi + 3G (GSM)"},
    {"iPad2,3",   "iPad 2 — Wi-Fi + 3G (CDMA)"},
    {"iPad2,4",   "iPad 2 — Wi-Fi"},
    {"iPad3,1",   "iPad (3rd gen) — Wi-Fi"},
    {"iPad3,2",   "iPad (3rd gen) — Wi-Fi + Cellular (CDMA)"},
    {"iPad3,3",   "iPad (3rd gen) — Wi-Fi + Cellular (GSM)"},
    {"iPad3,4",   "iPad (4th gen) — Wi-Fi"},
    {"iPad3,5",   "iPad (4th gen) — Wi-Fi + Cellular (GSM)"},
    {"iPad3,6",   "iPad (4th gen) — Wi-Fi + Cellular (CDMA)"},
    {"iPad6,11",  "iPad (5th gen) — Wi-Fi"},
    {"iPad6,12",  "iPad (5th gen) — Wi-Fi + Cellular"},
    {"iPad7,5",   "iPad (6th gen) — Wi-Fi"},
    {"iPad7,6",   "iPad (6th gen) — Wi-Fi + Cellular"},
    {"iPad7,11",  "iPad (7th gen) — Wi-Fi"},
    {"iPad7,12",  "iPad (7th gen) — Wi-Fi + Cellular"},
    {"iPad11,6",  "iPad (8th gen) — Wi-Fi"},
    {"iPad11,7",  "iPad (8th gen) — Wi-Fi + Cellular"},
    {"iPad12,1",  "iPad (9th gen) — Wi-Fi"},
    {"iPad12,2",  "iPad (9th gen) — Wi-Fi + Cellular"},
    {"iPad13,18", "iPad (10th gen) — Wi-Fi"},
    {"iPad13,19", "iPad (10th gen) — Wi-Fi + Cellular"},
    {"iPad15,7",  "iPad (11th gen) — Wi-Fi"},
    {"iPad15,8",  "iPad (11th gen) — Wi-Fi + Cellular"},

    // ── iPad mini ────────────────────────────────────────────────────────
    {"iPad2,5",   "iPad mini — Wi-Fi"},
    {"iPad2,6",   "iPad mini — Wi-Fi + 3G (GSM)"},
    {"iPad2,7",   "iPad mini — Wi-Fi + 3G (CDMA)"},
    {"iPad4,4",   "iPad mini 2 — Wi-Fi"},
    {"iPad4,5",   "iPad mini 2 — Wi-Fi + Cellular"},
    {"iPad4,6",   "iPad mini 2 — Wi-Fi + Cellular (China)"},
    {"iPad4,7",   "iPad mini 3 — Wi-Fi"},
    {"iPad4,8",   "iPad mini 3 — Wi-Fi + Cellular"},
    {"iPad4,9",   "iPad mini 3 — Wi-Fi + Cellular (China)"},
    {"iPad5,1",   "iPad mini 4 — Wi-Fi"},
    {"iPad5,2",   "iPad mini 4 — Wi-Fi + Cellular"},
    {"iPad11,1",  "iPad mini (5th gen) — Wi-Fi"},
    {"iPad11,2",  "iPad mini (5th gen) — Wi-Fi + Cellular"},
    {"iPad14,1",  "iPad mini (6th gen) — Wi-Fi"},
    {"iPad14,2",  "iPad mini (6th gen) — Wi-Fi + Cellular"},
    {"iPad16,1",  "iPad mini (A17 Pro) — Wi-Fi"},
    {"iPad16,2",  "iPad mini (A17 Pro) — Wi-Fi + Cellular"},

    // ── iPad Air ─────────────────────────────────────────────────────────
    {"iPad4,1",   "iPad Air — Wi-Fi"},
    {"iPad4,2",   "iPad Air — Wi-Fi + Cellular"},
    {"iPad4,3",   "iPad Air — Wi-Fi + Cellular (China)"},
    {"iPad5,3",   "iPad Air 2 — Wi-Fi"},
    {"iPad5,4",   "iPad Air 2 — Wi-Fi + Cellular"},
    {"iPad11,3",  "iPad Air (3rd gen) — Wi-Fi"},
    {"iPad11,4",  "iPad Air (3rd gen) — Wi-Fi + Cellular"},
    {"iPad13,1",  "iPad Air (4th gen) — Wi-Fi"},
    {"iPad13,2",  "iPad Air (4th gen) — Wi-Fi + Cellular"},
    {"iPad13,16", "iPad Air (5th gen) — Wi-Fi"},
    {"iPad13,17", "iPad Air (5th gen) — Wi-Fi + Cellular"},
    {"iPad14,8",  "iPad Air 11-inch (M2) — Wi-Fi"},
    {"iPad14,9",  "iPad Air 11-inch (M2) — Wi-Fi + Cellular"},
    {"iPad14,10", "iPad Air 13-inch (M2) — Wi-Fi"},
    {"iPad14,11", "iPad Air 13-inch (M2) — Wi-Fi + Cellular"},
    {"iPad15,3",  "iPad Air 11-inch (M3) — Wi-Fi"},
    {"iPad15,4",  "iPad Air 11-inch (M3) — Wi-Fi + Cellular"},
    {"iPad15,5",  "iPad Air 13-inch (M3) — Wi-Fi"},
    {"iPad15,6",  "iPad Air 13-inch (M3) — Wi-Fi + Cellular"},
    {"iPad16,8",  "iPad Air 11-inch (M4) — Wi-Fi"},
    {"iPad16,9",  "iPad Air 11-inch (M4) — Wi-Fi + Cellular"},
    {"iPad16,10", "iPad Air 13-inch (M4) — Wi-Fi"},
    {"iPad16,11", "iPad Air 13-inch (M4) — Wi-Fi + Cellular"},

    // ── iPad Pro ─────────────────────────────────────────────────────────
    {"iPad6,3",   "iPad Pro (9.7-inch) — Wi-Fi"},
    {"iPad6,4",   "iPad Pro (9.7-inch) — Wi-Fi + Cellular"},
    {"iPad6,7",   "iPad Pro 12.9-inch (1st gen) — Wi-Fi"},
    {"iPad6,8",   "iPad Pro 12.9-inch (1st gen) — Wi-Fi + Cellular"},
    {"iPad7,1",   "iPad Pro 12.9-inch (2nd gen) — Wi-Fi"},
    {"iPad7,2",   "iPad Pro 12.9-inch (2nd gen) — Wi-Fi + Cellular"},
    {"iPad7,3",   "iPad Pro (10.5-inch) — Wi-Fi"},
    {"iPad7,4",   "iPad Pro (10.5-inch) — Wi-Fi + Cellular"},
    {"iPad8,1",   "iPad Pro 11-inch (1st gen) — Wi-Fi"},
    {"iPad8,2",   "iPad Pro 11-inch (1st gen) — Wi-Fi"},
    {"iPad8,3",   "iPad Pro 11-inch (1st gen) — Wi-Fi + Cellular"},
    {"iPad8,4",   "iPad Pro 11-inch (1st gen) — Wi-Fi + Cellular"},
    {"iPad8,5",   "iPad Pro 12.9-inch (3rd gen) — Wi-Fi"},
    {"iPad8,6",   "iPad Pro 12.9-inch (3rd gen) — Wi-Fi"},
    {"iPad8,7",   "iPad Pro 12.9-inch (3rd gen) — Wi-Fi + Cellular"},
    {"iPad8,8",   "iPad Pro 12.9-inch (3rd gen) — Wi-Fi + Cellular"},
    {"iPad8,9",   "iPad Pro 11-inch (2nd gen) — Wi-Fi"},
    {"iPad8,10",  "iPad Pro 11-inch (2nd gen) — Wi-Fi + Cellular"},
    {"iPad8,11",  "iPad Pro 12.9-inch (4th gen) — Wi-Fi"},
    {"iPad8,12",  "iPad Pro 12.9-inch (4th gen) — Wi-Fi + Cellular"},
    {"iPad13,4",  "iPad Pro 11-inch (3rd gen) — Wi-Fi"},
    {"iPad13,5",  "iPad Pro 11-inch (3rd gen) — Wi-Fi + Cellular (US)"},
    {"iPad13,6",  "iPad Pro 11-inch (3rd gen) — Wi-Fi + Cellular (Global)"},
    {"iPad13,7",  "iPad Pro 11-inch (3rd gen) — Wi-Fi + Cellular (China)"},
    {"iPad13,8",  "iPad Pro 12.9-inch (5th gen) — Wi-Fi"},
    {"iPad13,9",  "iPad Pro 12.9-inch (5th gen) — Wi-Fi + Cellular (US)"},
    {"iPad13,10", "iPad Pro 12.9-inch (5th gen) — Wi-Fi + Cellular (Global)"},
    {"iPad13,11", "iPad Pro 12.9-inch (5th gen) — Wi-Fi + Cellular (China)"},
    {"iPad14,3",  "iPad Pro 11-inch (4th gen) — Wi-Fi"},
    {"iPad14,4",  "iPad Pro 11-inch (4th gen) — Wi-Fi + Cellular"},
    {"iPad14,5",  "iPad Pro 12.9-inch (6th gen) — Wi-Fi"},
    {"iPad14,6",  "iPad Pro 12.9-inch (6th gen) — Wi-Fi + Cellular"},
    {"iPad16,3",  "iPad Pro 11-inch (M4) — Wi-Fi"},
    {"iPad16,4",  "iPad Pro 11-inch (M4) — Wi-Fi + Cellular"},
    {"iPad16,5",  "iPad Pro 13-inch (M4) — Wi-Fi"},
    {"iPad16,6",  "iPad Pro 13-inch (M4) — Wi-Fi + Cellular"},
    {"iPad17,1",  "iPad Pro 11-inch (M5) — Wi-Fi"},
    {"iPad17,2",  "iPad Pro 11-inch (M5) — Wi-Fi + Cellular"},
    {"iPad17,3",  "iPad Pro 13-inch (M5) — Wi-Fi"},
    {"iPad17,4",  "iPad Pro 13-inch (M5) — Wi-Fi + Cellular"},
};

const char* resolve(const char* raw_id) {
    for (const auto& entry : k_models) {
        if (strcmp(raw_id, entry.id) == 0) return entry.name;
    }
    return nullptr;  // unknown identifier — caller falls back to raw_id itself
}

} // namespace iphone_model
