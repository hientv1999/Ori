#pragma once
#include <stdint.h>

namespace ancs_icons {

// Returns the brand fill colour (0xRRGGBB) for the given icon token.
// Tokens match the mock_data / ANCS bundle-id registry
// ("gmail", "slack", "whatsapp", …).
// Unknown or null tokens return 0x555555 (neutral grey).
uint32_t color(const char* token);

} // namespace ancs_icons
