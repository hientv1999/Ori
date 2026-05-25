#include "assets/ancs_icons.h"

#include <string.h>

namespace ancs_icons {

namespace {

struct Entry { const char* token; uint32_t rgb; };

// Brand colours per app token. Approximate — will be refined in M8.
// Entries cover every token in mock_data.cpp and ancs_client.cpp's bundle map.
static const Entry k_table[] = {
    { "gmail",     0xEA4335 },
    { "messenger", 0x006AFF },
    { "instagram", 0xE1306C },
    { "facebook",  0x1877F2 },
    { "whatsapp",  0x25D366 },
    { "telegram",  0x26A5E4 },
    { "signal",    0x3A76F0 },
    { "slack",     0x4A154B },
    { "discord",   0x5865F2 },
    { "twitter",   0x1DA1F2 },
    { "linkedin",  0x0A66C2 },
    { "tiktok",    0xFF0050 },
    { "snapchat",  0xFFFC00 },
    { "zoom",      0x2D8CFF },
    { "teams",     0x464EB8 },
    { "outlook",   0x0078D4 },
    { "sms",       0x3DC95E },
    { "spotify",   0x1DB954 },
    { "youtube",   0xFF0000 },
    { "wechat",    0x07C160 },
    { "phone",     0x34C759 },
    { "voicemail", 0x5C5CE0 },
    { "line",      0x00B900 },
};

} // namespace

uint32_t color(const char* token) {
    if (!token) return 0x555555;
    for (const auto& e : k_table) {
        if (strcmp(e.token, token) == 0) return e.rgb;
    }
    return 0x555555;
}

} // namespace ancs_icons
