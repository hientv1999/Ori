#include "ble/ancs_bundle_map.h"

#include <string.h>

namespace ancs_bundle_map {

static const Entry k_map[] = {
    { "com.google.Gmail",               "gmail",       "Gmail"       },
    { "com.facebook.Messenger",         "messenger",   "Messenger"   },
    { "com.burbn.instagram",            "instagram",   "Instagram"   },
    { "com.facebook.Facebook",          "facebook",    "Facebook"    },
    { "net.whatsapp.WhatsApp",          "whatsapp",    "WhatsApp"    },
    { "com.tinyspeck.chatlyio",         "slack",       "Slack"       },
    { "com.atebits.Tweetie2",           "twitter",     "X"           },
    { "com.microsoft.skype.teams",      "teams",       "Teams"       },
    { "com.apple.MobileSMS",            "sms",         "Messages"    },
    { "com.apple.mobilephone",          "phone",       "Phone"       },
    { "com.hammerandchisel.discord",    "discord",     "Discord"     },
    { "ph.telegra.Telegraph",           "telegram",    "Telegram"    },
    { "com.google.ios.youtube",         "youtube",       "YouTube"       },
    { "com.google.ios.youtubemusic",    "youtube_music", "YouTube Music" },
    { "com.zhiliaoapp.musically",       "tiktok",      "TikTok"      },
    { "com.ss.iphone.ugc.Ame",          "tiktok",      "TikTok"      },
    { "com.spotify.client",             "spotify",     "Spotify"     },
    { "com.tencent.xin",                "wechat",      "WeChat"      },
    { "jp.naver.line",                  "line",        "LINE"        },
    { "us.zoom.videomeetings",          "zoom",        "Zoom"        },
    { "com.microsoft.office.outlook",   "outlook",     "Outlook"     },
    { "com.toyopagroup.picaboo",        "snapchat",    "Snapchat"    },
    { "com.google.hangouts.meet",       "google_meet", "Google Meet" },
    { "com.apple.facetime",             "facetime",    "FaceTime"    },
    { "com.linkedin.LinkedIn",          "linkedin",    "LinkedIn"    },
    { "com.reddit.Reddit",              "reddit",      "Reddit"      },
    { "com.burbn.barcelona",            "threads",     "Threads"     },
    { "tv.twitch.mobile.watchlive",     "twitch",      "Twitch"      },
    { "com.ubercab.UberClient",         "uber",        "Uber"        },
    { "com.apple.Music",                "apple_music", "Apple Music" },
    { "com.amazon.Amazon",              "amazon",      "Amazon"      },
    { "com.viber",                      "viber",       "Viber"       },
    { "com.anthropic.claude",           "claude",      "Claude"      },
    { "com.openai.chat",                "chatgpt",     "ChatGPT"     },
    { "com.google.Maps",                "google_map",    "Google Maps"   },
    { "com.google.photos",              "google_photos", "Google Photos" },
    { "com.apple.Health",               "health",        "Health"        },
    { "com.apple.mobilecal",            "apple_calendar",          "Calendar"               },
    { "com.apple.findmy",               "apple_findmy",            "Find My"                },
    { "com.apple.mobilemail",           "apple_mail",              "Mail"                   },
    { "com.apple.Maps",                 "apple_maps",              "Maps"                   },
    { "com.apple.reminders",            "apple_reminders",         "Reminders"              },
    { "com.apple.Passbook",             "apple_wallet",            "Wallet"                 },
    { "com.github.stormbreaker.prod",   "github",                  "GitHub"                 },
    { "com.google.authenticator",       "google_authenticator",    "Google Authenticator"   },
    { "com.azure.authenticator",        "microsoft_authenticator", "Microsoft Authenticator"},
    { "notion.id",                      "notion",                  "Notion"                 },
    { "com.venmo.Venmo",                "venmo",                   "Venmo"                  },
    { "com.skype.skype",                "skype",                   "Skype"                  },
    { "com.paypal.PPClient",            "paypal",                  "PayPal"                 },
    { "com.wealthsimple.wealthsimple",  "wealthsimple",            "Wealthsimple"           },
    // Bundle IDs below are best-effort guesses EXCEPT where marked CONFIRMED,
    // which means the id was read off a real notification's own
    // "[ancs] attr ... app=<id>" log line. A wrong id costs only the brand
    // icon (the notification still shows, with its category fallback glyph)
    // — and, if the filter is on AppPassthrough, membership of the allowlist,
    // since an unresolved token can never match a picked app.
    //
    // RBC is the cautionary example: the guess com.rbc.mobile.ios was wrong,
    // and the real id turned out to be com.rbc.mobilebanking. Note it does
    // NOT follow the com.<brand>.mobile.ios shape several of these assume, so
    // treat the remaining guesses as genuinely unknown rather than likely.
    { "com.vng.zalo",                   "zalo",                    "Zalo"                   },
    { "com.td.TDCTMobile",              "td",                      "TD"                     },
    { "com.bmo.mobilebanking",          "bmo",                     "BMO"                    },
    { "com.rbc.mobilebanking",          "rbc",                     "RBC"                    },   // CONFIRMED from a real notification
    { "com.scotiabank.banking",         "scotiabank",              "Scotiabank"             },
    { "com.cibc.ios.mobilebanking",     "cibc",                    "CIBC"                   },
    // iOS Settings — software-update / storage / account alerts. NOTE this is
    // Settings specifically, not "system notifications" as a class: other
    // Apple system alerts come from their own bundles (com.apple.springboard,
    // com.apple.datausage, ...) and each needs its own row. ANCS has no
    // "system" CategoryID to key on — these arrive as category 0 (Other),
    // same as any miscellaneous third-party notification.
    { "com.apple.Preferences",          "apple_settings",          "Settings"               },   // CONFIRMED from a real notification
};
static const size_t k_map_count = sizeof(k_map) / sizeof(k_map[0]);

const Entry* lookup(const char* bundle_id) {
    if (!bundle_id || !bundle_id[0]) return nullptr;
    for (size_t i = 0; i < k_map_count; ++i) {
        if (strcmp(k_map[i].bundle_id, bundle_id) == 0) return &k_map[i];
    }
    return nullptr;
}

} // namespace ancs_bundle_map
