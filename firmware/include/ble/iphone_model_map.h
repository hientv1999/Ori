#pragma once

// Apple's internal hardware identifier (Device Information Service, Model
// Number String 0x2A24, e.g. "iPhone18,2") -> marketing name ("iPhone 17 Pro
// Max"), for ancs_client.cpp's iPhone Info modal. Pure static lookup data,
// pulled out of ancs_client.cpp for the same reason as ancs_bundle_map.h —
// no shared mutable state, so this table can grow (a new iPhone every year)
// independently of the ANCS protocol/queue logic that file owns.
//
// Source: the community-maintained identifier list (github.com/adamawolf/
// FreeStreamer's companion gist, gist.github.com/adamawolf/3048717), the same
// one widely referenced for this exact purpose; cross-checked against Apple's
// own "Identify your iPhone model" support page and EveryMac's lookup for the
// newest (iPhone18,x) entries. Regional/carrier variants that share one
// marketing name (e.g. iPhone5,1 GSM vs iPhone5,2 GSM+CDMA are both plain
// "iPhone 5") are collapsed to that one name — this is a display label, not
// a technical identifier.

namespace iphone_model {

// Returns the marketing name for `raw_id`, or nullptr if unrecognized (a
// model newer than this table) — caller falls back to the raw identifier
// itself rather than guessing or showing nothing.
const char* resolve(const char* raw_id);

} // namespace iphone_model
