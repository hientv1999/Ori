#include "mock_data.h"

namespace mock_data {

namespace {

const Profile k_profile = {
    // Long mock name exercises the 2-line wrap path now that the name font
    // scales to font_time() (30 px) — see widget_profile_card.cpp.
    "Everstorm Dominion",
    "Founder, Ori",
    "ED",
    "everstorm@ori.app",
    "+1 (415) 555 0192",
};

const char* k_ble_name  = "Ori-XT-9F";
const char* k_passkey   = "476 218";

const Pto k_pto = {
    "Lisbon, Portugal",
    "May 13 – May 21",
};

const Time k_now = {
    "14:30",
    "Wed, May 14",
    "WEDNESDAY, MAY 14",
    "PM",
    "2",
    "30",
};

// TODAY_MEETINGS verbatim.
// Field order: { start, end, title, loc, org, overlap, in_progress }.
// Mock "now" is 14:30 → the 14:00-15:00 row is marked in_progress so the
// device shows a danger-red highlight on the meeting the user is currently
// supposed to be attending.
const Meeting k_today[] = {
    { "09:30", "10:00", "Daily standup",            "Conf Room A", "Priya N.",  false, false },
    { "10:30", "11:30", "Industrial design review", "Studio",      "Marcus L.", false, false },
    { "13:00", "13:45", "Orion app sync",           "Zoom",        "Hannah K.", false, false },
    { "14:00", "15:00", "Investor update prep",     "Office",      "Xander T.", false, true  },
    { "15:30", "16:00", "Quick sync with marketing","Studio",      "Sam R.",    false, false },
    { "16:30", "17:00", "Q4 budget review",
      "Virtual \xe2\x80\x94 Microsoft Teams Conference Room B",
      "Dr. Christopher Vandenbrook",                                false, false },
};

// OVERLAP_MEETINGS verbatim.
const Meeting k_overlap[] = {
    { "10:00", "11:00", "Firmware architecture review", "Conf Room A", "Marcus L.", true,  false },
    { "10:00", "10:30", "Quick chat with Priya",        "Coffee bar",  "Priya N.",  true,  false },
    { "10:30", "11:30", "Vendor call — display panels", "Zoom",        "Hannah K.", true,  false },
};

// LONG_TITLE_MEETINGS verbatim.
const Meeting k_long_title[] = {
    { "09:00", "10:00",
      "Q3 roadmap planning across firmware, Orion, mobile, and operations",
      "Conf Room A", "Priya N.", false, false },
    { "11:00", "12:00",
      "Industrial design review — chassis, materials, and tooling decisions for the pilot run",
      "Studio", "Marcus L.", false, false },
    { "14:00", "15:30", "Investor preview", "Office", "Xander T.", false, true },
};

// OVERLAP_LONG_MEETINGS verbatim.
const Meeting k_overlap_long[] = {
    { "10:00", "11:00",
      "Strategic planning session for Q4 — deep dive into roadmap and prioritisation",
      "Studio", "Marcus L.", true,  false },
    { "10:00", "10:45",
      "Manufacturing partner kickoff with the Shenzhen vendor",
      "Zoom", "Hannah K.", true,  false },
    { "11:00", "12:00", "Standup", "Studio", "Priya N.", false, false },
};

// LONG_LIST_MEETINGS verbatim.
const Meeting k_long_list[] = {
    { "09:00", "09:30", "Daily standup",                                                 "Conf Room A", "Priya N.",  false, false },
    { "09:30", "10:30", "Firmware architecture review with hardware team",               "Conf Room A", "Marcus L.", false, false },
    { "10:30", "11:00", "Orion app sync",                                                "Zoom",        "Hannah K.", false, false },
    { "11:00", "12:00", "Industrial design review",                                      "Studio",      "Marcus L.", false, false },
    { "13:00", "13:45", "Vendor call — display panels and cover glass options",          "Zoom",        "Hannah K.", false, false },
    { "14:00", "15:00", "One-on-one with Priya",                                         "Coffee bar",  "Priya N.",  false, true  },
    { "15:00", "15:30", "Investor update prep",                                          "Office",      "Xander T.", false, false },
    { "15:30", "16:00", "Quick sync with marketing",                                     "Studio",      "Sam R.",    false, false },
    { "16:00", "17:00", "Weekly retro",                                                  "Conf Room B", "Hannah K.", false, false },
};

// Per-app ANCS notification mocks. Each entry maps to the string token used
// by both the status bar icon and the detail overlay.
// Token list and notification list must stay index-aligned.

static const char* k_ancs_tokens[] = {
    "gmail",
    "messenger",
    "instagram",
    "facebook",
    "whatsapp",
    "telegram",
    "signal",
    "slack",
    "discord",
    "twitter",
    "linkedin",
    "tiktok",
    "snapchat",
    "zoom",
    "teams",
    "outlook",
    "sms",
    "spotify",
    "youtube",
    "wechat",
    "phone",
    "voicemail",
    "line",
};

const AncsNotification k_ancs_notifications[] = {
    { "Gmail",      "Priya Nair",
      "Hey, have you had a chance to review the firmware PR? "
      "The team is waiting on approval before they can merge and kick off the build.",
      "2 min ago" },
    { "Messenger",  "Marcus Lee",
      "Studio is booked for the design review. I'll send the invite now "
      "\xe2\x80\x94 can you confirm you're free at 10:30? Also, Hannah said she "
      "needs at least 20 minutes at the end to walk through the chassis tolerances "
      "with the vendor rep, so I've blocked the room until 12:00 just in case.",
      "5 min ago" },
    { "Instagram",  "New activity",
      "oridevice and 47 others liked your photo.",
      "18 min ago" },
    { "Facebook",   "Hannah Kim commented",
      "Great progress on the display calibration \xe2\x80\x94 the green channel fix looks solid. "
      "Let\xe2\x80\x99s review the pin map again before M3.",
      "1 hr ago" },
    { "WhatsApp",   "Sam Rivera",
      "Just landed in Lisbon \xe2\x80\x94 the office address is on the calendar invite. "
      "See you at the venue at 18:30.",
      "3 min ago" },
    { "Telegram",   "Ori Dev Channel",
      "New build pushed to staging: firmware v1.2.0-rc3. "
      "Please flash and run the BLE pairing smoke test before EOD.",
      "7 min ago" },
    { "Signal",     "Dr. Vandenbrook",
      "The NDA is signed. I\xe2\x80\x99ll send the full spec sheet over secure mail this afternoon.",
      "12 min ago" },
    { "Slack",      "#firmware \xe2\x80\x94 Hannah Kim",
      "The GT911 checksum fix is in. Marking the touch driver ticket as done. "
      "Heads up: the config write is only safe after Wire is up \xe2\x80\x94 see the comment in init().",
      "Just now" },
    { "Discord",    "hardware-dev \xe2\x80\x94 Xander T.",
      "I measured the backlight current at 180 mA. Should be fine on a 500 mA adapter "
      "even with peak display load. Posting the scope capture in the thread.",
      "4 min ago" },
    { "Twitter",    "New mention",
      "@oridevice The always-on desk display idea is really compelling. "
      "Any plans to make it open hardware?",
      "22 min ago" },
    { "LinkedIn",   "You have a new connection",
      "Marcus Lee accepted your connection request. "
      "You now have 3 mutual connections.",
      "45 min ago" },
    { "TikTok",     "New follower",
      "oridevice_official gained 128 new followers today.",
      "1 hr ago" },
    { "Snapchat",   "Priya Nair",
      "Priya sent you a snap.",
      "10 min ago" },
    { "Zoom",       "Meeting starting soon",
      "Your meeting \xe2\x80\x94 Q4 Investor Preview \xe2\x80\x94 starts in 5 minutes. "
      "Join link is in the invite.",
      "5 min ago" },
    { "Teams",      "Hannah Kim",
      "I\xe2\x80\x99ve shared the updated chassis render in the Design channel. "
      "The new corner radius matches the prototype spec exactly.",
      "8 min ago" },
    { "Outlook",    "Vendor Invoice \xe2\x80\x94 Shenzhen Components",
      "Invoice #SZX-2024-0412 is ready for review. "
      "Amount due: $14,280. Payment terms: Net 30.",
      "2 hr ago" },
    { "Messages",   "Xander T.",
      "The enclosure samples arrived. Corner finish is perfect \xe2\x80\x94 "
      "bring them to the design review tomorrow.",
      "30 min ago" },
    { "Spotify",    "New release",
      "Angelo Badalamenti released a new track: "
      "\xe2\x80\x9c" "Industrial Symphony No. 2\xe2\x80\x9d. Available now.",
      "1 hr ago" },
    { "YouTube",    "New upload",
      "Ori Device posted: \xe2\x80\x9c" "Building a Desk Status Display from Scratch"
      "\xe2\x80\x9d \xe2\x80\x94 26 min",
      "3 hr ago" },
    { "WeChat",     "Marcus Lee",
      "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95 \xe2\x80\x94 "
      "Translation: \xe2\x80\x9cSee you at the factory on Thursday.\xe2\x80\x9d",
      "35 min ago" },
    { "Phone",      "Incoming call",
      "Priya Nair is calling.",
      "Just now" },
    { "Voicemail",  "New voicemail",
      "You have 1 new voicemail from +1 (415) 555 0199.",
      "15 min ago" },
    { "LINE",       "Sam Rivera",
      "The venue confirmed the booking for Thursday evening.",
      "20 min ago" },
};

static_assert(
    sizeof(k_ancs_tokens) / sizeof(k_ancs_tokens[0]) ==
    sizeof(k_ancs_notifications) / sizeof(k_ancs_notifications[0]),
    "k_ancs_tokens and k_ancs_notifications must have the same number of entries");

static const AncsNotification k_ancs_fallback = {
    "Notification", "Notification", "No preview available.", "Just now"
};

// Default ANCS state shown in the prototype meeting-list screen.
// 7 queued notifications — only the first 5 are visible in the status bar.
// Dismissing any of the first 5 shifts the queue left and reveals the next.
AncsConfig k_ancs = {
    { "gmail", "slack", "whatsapp", "teams", "messenger", "instagram", "discord" },
    7,
    true,
};

// Now-playing mock. Long title exercises the ellipsis path.
Media k_media = {
    "Industrial Symphony No. 1 — The Dream of the Brokenhearted Woman",
    "Angelo Badalamenti",
    /*has_media=*/true,
    /*volume=*/65,
    /*position_s=*/83,
    /*duration_s=*/245,
    /*can_seek=*/true,
};
bool k_playing = false;  // start paused so the device shows the dim-+-play overlay

// Default shortcut config — same icons the HTML prototype mocks.
const ShortcutSlot k_shortcuts[SHORTCUT_COUNT] = {
    { "vol-mute"   },   // Mute audio
    { "mic-mute"   },   // Mute mic
    { "screenshot" },   // Screen capture
};

} // namespace

const Profile& profile()         { return k_profile; }
const char*    ble_name()        { return k_ble_name; }
const char*    passkey()         { return k_passkey; }
const Pto&     pto()             { return k_pto; }
const Time&    now()             { return k_now; }
const char*    synced_pill_text(){ return "SYNCED · 12 min ago"; }

MeetingList meetings()              { return { k_today,        sizeof(k_today)        / sizeof(Meeting) }; }
MeetingList meetings_overlap()      { return { k_overlap,      sizeof(k_overlap)      / sizeof(Meeting) }; }
MeetingList meetings_long_title()   { return { k_long_title,   sizeof(k_long_title)   / sizeof(Meeting) }; }
MeetingList meetings_overlap_long() { return { k_overlap_long, sizeof(k_overlap_long) / sizeof(Meeting) }; }
MeetingList meetings_long_list()    { return { k_long_list,    sizeof(k_long_list)    / sizeof(Meeting) }; }

const AncsConfig& ancs_config() { return k_ancs; }

void set_ancs_config(const AncsConfig& cfg) {
    k_ancs = cfg;
    // Clamp to the queue depth — prevents out-of-bounds reads if the caller
    // passes a count larger than the icons[] array. The display cap
    // (MAX_ANCS_ICONS = 5) is enforced separately in widget_status_bar::refresh().
    if (k_ancs.count > MAX_ANCS_NOTIFICATIONS) k_ancs.count = MAX_ANCS_NOTIFICATIONS;
}

const AncsNotification& ancs_notification(const char* token) {
    if (token) {
        for (size_t i = 0; i < sizeof(k_ancs_tokens) / sizeof(k_ancs_tokens[0]); ++i) {
            const char* t = k_ancs_tokens[i];
            bool match = true;
            for (int j = 0; t[j] || token[j]; ++j) {
                if (t[j] != token[j]) { match = false; break; }
            }
            if (match) return k_ancs_notifications[i];
        }
    }
    return k_ancs_fallback;
}

const Media& media()                  { return k_media; }
bool         media_playing()          { return k_playing; }
void         set_media_playing(bool playing) { k_playing = playing; }
void         set_media_volume(int v)  { if (v < 0) v = 0; if (v > 100) v = 100; k_media.volume = v; }
const ShortcutSlot* shortcuts()       { return k_shortcuts; }

} // namespace mock_data
