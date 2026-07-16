#pragma once

// Apple's internal hardware identifier (Device Information Service, Model
// Number String 0x2A24, e.g. "iPhone18,2" or "iPad14,3") -> marketing name
// ("iPhone 17 Pro Max", "iPad Pro 11-inch (4th generation)"), for
// ancs_client.cpp's iPhone/iPad Info modal. Ori bonds with either an iPhone
// or an iPad in the same ANCS slot (connectivity.md) — both product lines
// use the same identifier scheme (a name prefix + comma-separated generation
// pair), so one table and one resolve() cover both; the namespace/file are
// still named for the iPhone case that came first, but nothing here is
// iPhone-specific. Pure static lookup data, pulled out of ancs_client.cpp for
// the same reason as ancs_bundle_map.h — no shared mutable state, so this
// table can grow (a new iPhone/iPad every year) independently of the ANCS
// protocol/queue logic that file owns.
//
// Source: the community-maintained identifier list (github.com/adamawolf/
// FreeStreamer's companion gist, gist.github.com/adamawolf/3048717), the same
// one widely referenced for this exact purpose; cross-checked against Apple's
// own "Identify your iPhone/iPad model" support pages, AppleDB
// (appledb.dev/device-selection), and EveryMac's lookup for the newest
// entries. Regional/carrier/storage variants that share one marketing name
// (e.g. iPhone5,1 GSM vs iPhone5,2 GSM+CDMA are both plain "iPhone 5") are
// collapsed to that one name — this is a display label, not a technical
// identifier.

namespace iphone_model {

// Returns the marketing name for `raw_id` (iPhone or iPad), or nullptr if
// unrecognized (a model newer than this table) — caller falls back to the
// raw identifier itself rather than guessing or showing nothing. Since every
// raw identifier already starts with "iPhone" or "iPad" (Apple's own naming),
// that fallback still lets ancs_client::phone_kind_word() tell the two
// families apart even for a model this table hasn't caught up to yet.
//
// Entries that would otherwise collapse to an identical name across sibling
// identifiers (e.g. every iPad2,x is "iPad 2") instead carry a short
// connectivity/radio/region suffix so ancs_client's caller can tell them
// apart — "iPad 2 — Wi-Fi + 3G (GSM)" vs "iPad 2 — Wi-Fi + 3G (CDMA)" vs
// plain "iPad 2 — Wi-Fi". Longest current entries land around 58 of the
// 63 usable bytes in ancs_client.cpp's g_phone_device_type[64] — check
// against that budget before adding a longer one.
const char* resolve(const char* raw_id);

} // namespace iphone_model
