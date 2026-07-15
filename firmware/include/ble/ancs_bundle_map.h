#pragma once

// Bundle ID -> icon token + display name, for apps ancs_client.cpp resolves
// out of an ANCS AppIdentifier. Pure static lookup data with no shared
// mutable state — pulled out of ancs_client.cpp so its ~50-entry table
// doesn't crowd the file that owns the actual ANCS protocol/queue logic.
// Adding a new app is a one-line addition here; see firmware.md's supported-
// token list for the icon side of this mapping (assets/ancs_icons.h/cpp).

namespace ancs_bundle_map {

struct Entry {
    const char* bundle_id;
    const char* token;
    const char* name;     // human-readable app name for the detail modal
};

// Returns the matching entry for `bundle_id`, or nullptr if unmapped
// (caller falls back to the "unknown" token + a category fallback icon).
const Entry* lookup(const char* bundle_id);

} // namespace ancs_bundle_map
