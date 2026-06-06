// Ori ANCS client — Apple Notification Center Service subscriber.
//
// ANCS service UUID:  7905F431-B5CE-4E99-A40F-4B1E122D00D0
// Notification Source (NS): 9FBF120D-6301-42D9-8C58-25E699A21DBD
// Control Point (CP):       69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9
// Data Source (DS):         22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB
//
// Notification-source event byte layout (8 bytes):
//   0: EventID      (0=Added, 1=Modified, 2=Removed)
//   1: EventFlags
//   2: CategoryID
//   3: CategoryCount
//   4-7: NotificationUID (uint32 LE)
//
// GetNotificationAttributes command (sent to CP):
//   0: CommandID = 0x00
//   1-4: NotificationUID (uint32 LE)
//   5+: AttributeID list (0x01=Title, 0x03=Message, with length for 01 and 03)
//
// Icons only — notification content displayed only on user tap via modal.

#include "ble/ancs_client.h"
#include "ble/ble_manager.h"

#include <Arduino.h>
#include "ori_log.h"
#include <NimBLEDevice.h>
#include <NimBLEClient.h>
#include <string.h>

#include "mock_data.h"
#include "state_machine.h"
#include "assets/ancs_icons.h"

// ── ANCS UUIDs ────────────────────────────────────────────────────────────

#define ANCS_SVC_UUID "7905F431-B5CE-4E99-A40F-4B1E122D00D0"
#define ANCS_NS_UUID  "9FBF120D-6301-42D9-8C58-25E699A21DBD"
#define ANCS_CP_UUID  "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9"
#define ANCS_DS_UUID  "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB"

// ── Bundle ID → icon token mapping ───────────────────────────────────────

struct BundleMap {
    const char* bundle_id;
    const char* token;
};

static const BundleMap k_bundle_map[] = {
    { "com.google.gmail.iphone",        "gmail"       },
    { "com.facebook.Messenger",         "messenger"   },
    { "com.burbn.instagram",            "instagram"   },
    { "com.facebook.Facebook",          "facebook"    },
    { "net.whatsapp.WhatsApp",          "whatsapp"    },
    { "com.tinyspeck.chatlyio",         "slack"       },
    { "com.atebits.Tweetie2",           "twitter"     },
    { "com.microsoft.teams",            "teams"       },
    { "com.apple.MobileSMS",            "sms"         },
    { "com.apple.mobilephone",          "phone"       },
    { "com.hammerandchisel.discord",    "discord"     },
    { "ph.telegra.Telegraph",           "telegram"    },
    { "com.google.ios.youtube",         "youtube"     },
    { "com.zhiliaoapp.musically",       "tiktok"      },
    { "com.spotify.client",             "spotify"     },
    { "com.tencent.xin",                "wechat"      },
    { "jp.naver.line",                  "line"        },
    { "us.zoom.videomeetings",          "zoom"        },
    { "com.microsoft.office.outlook",   "outlook"     },
    { "com.toyopagroup.picaboo",        "snapchat"    },
    { "com.google.hangouts.meet",       "google_meet" },
    { "com.apple.facetime",             "facetime"    },
    { "com.linkedin.LinkedIn",          "linkedin"    },
    { "com.reddit.Reddit",              "reddit"      },
    { "com.burbn.threads",              "threads"     },
    { "tv.twitch.mobile.watchlive",     "twitch"      },
    { "com.ubercab.UberClient",         "uber"        },
    { "com.apple.Music",                "apple_music" },
    { "com.amazon.Amazon",              "amazon"      },
};
static const size_t k_bundle_map_count =
    sizeof(k_bundle_map) / sizeof(k_bundle_map[0]);

static const char* resolve_token(const char* bundle_id) {
    if (!bundle_id || !bundle_id[0]) return nullptr;
    for (size_t i = 0; i < k_bundle_map_count; ++i) {
        if (strcmp(k_bundle_map[i].bundle_id, bundle_id) == 0) {
            return k_bundle_map[i].token;
        }
    }
    return nullptr;
}

namespace {

// ── Live queue ────────────────────────────────────────────────────────────

ancs_client::QueueEntry g_queue[mock_data::MAX_ANCS_NOTIFICATIONS];
size_t                  g_queue_count = 0;

// ── Pending attributes ────────────────────────────────────────────────────

ancs_client::NotificationInfo g_pending_info;
bool                          g_pending_valid = false;

// ── BLE handles ──────────────────────────────────────────────────────────

uint16_t g_conn_handle = 0xFFFF;
NimBLEClient*           g_client      = nullptr;
NimBLERemoteService*    g_ancs_svc    = nullptr;
NimBLERemoteCharacteristic* g_ns_char = nullptr;
NimBLERemoteCharacteristic* g_cp_char = nullptr;
NimBLERemoteCharacteristic* g_ds_char = nullptr;

// ── DS reassembly ─────────────────────────────────────────────────────────

// The Data Source delivers attribute data across multiple notifications.
// We accumulate until we see all requested attributes.
uint8_t  g_ds_buf[1024];
size_t   g_ds_len = 0;
uint32_t g_pending_uid = 0;

// ─────────────────────────────────────────────────────────────────────────
// Queue helpers
// ─────────────────────────────────────────────────────────────────────────

static void queue_add(uint32_t uid, const char* token) {
    // Check for duplicate UID.
    for (size_t i = 0; i < g_queue_count; ++i) {
        if (g_queue[i].uid == uid) return;
    }

    if (g_queue_count >= mock_data::MAX_ANCS_NOTIFICATIONS) {
        // Displace oldest (FIFO).
        memmove(&g_queue[0], &g_queue[1],
                (mock_data::MAX_ANCS_NOTIFICATIONS - 1) * sizeof(ancs_client::QueueEntry));
        g_queue_count = mock_data::MAX_ANCS_NOTIFICATIONS - 1;
    }

    g_queue[g_queue_count].uid = uid;
    strncpy(g_queue[g_queue_count].icon_token,
            token ? token : "unknown",
            sizeof(g_queue[g_queue_count].icon_token) - 1);
    g_queue[g_queue_count].icon_token[sizeof(g_queue[g_queue_count].icon_token) - 1] = '\0';
    g_queue_count++;

    // Sync icon state to mock_data so the status bar widget refreshes.
    mock_data::AncsConfig cfg = {};
    cfg.phone_connected = true;
    cfg.count = g_queue_count;
    for (size_t i = 0; i < g_queue_count && i < mock_data::MAX_ANCS_NOTIFICATIONS; ++i) {
        cfg.icons[i] = g_queue[i].icon_token;
    }
    mock_data::set_ancs_config(cfg);

    state_machine::set_phone_connected(true);
    LOG("[ancs] queued uid=%u token=%s count=%u\n",
                   (unsigned)uid, token ? token : "?", (unsigned)g_queue_count);
}

static void queue_remove(uint32_t uid) {
    for (size_t i = 0; i < g_queue_count; ++i) {
        if (g_queue[i].uid == uid) {
            memmove(&g_queue[i], &g_queue[i + 1],
                    (g_queue_count - i - 1) * sizeof(ancs_client::QueueEntry));
            g_queue_count--;

            mock_data::AncsConfig cfg = {};
            cfg.phone_connected = true;
            cfg.count = g_queue_count;
            for (size_t j = 0; j < g_queue_count; ++j) {
                cfg.icons[j] = g_queue[j].icon_token;
            }
            mock_data::set_ancs_config(cfg);

            LOG("[ancs] removed uid=%u count=%u\n",
                           (unsigned)uid, (unsigned)g_queue_count);
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// NS callback (notification events)
// ─────────────────────────────────────────────────────────────────────────

// Forward declare
static void notify_ns_cb(NimBLERemoteCharacteristic* c,
                          uint8_t* data, size_t len, bool is_notify);
static void notify_ds_cb(NimBLERemoteCharacteristic* c,
                          uint8_t* data, size_t len, bool is_notify);

} // namespace

namespace ancs_client {

void init() {
    LOG("[ancs] client init\n");
    // NimBLE client profile registration happens in on_iphone_connected().
}

void on_iphone_connected(uint16_t conn_handle) {
    LOG("[ancs] iPhone connected, handle=%u\n", (unsigned)conn_handle);
    g_conn_handle = conn_handle;

    // In NimBLE 2.5, the server hosts the ANCS client connection.
    // We use the server's getClient() method.
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server) {
        g_client = server->getClient(conn_handle);
    }
    if (!g_client) {
        LOG("[ancs] WARN: no NimBLEClient for ANCS handle (expected for server-only builds)\n");
        // ANCS uses the remote service discovery API from a NimBLEClient perspective.
        // On ESP32-S3 with both peripheral+central roles enabled, this will work.
        // If central role is not compiled in, ANCS discovery will be skipped gracefully.
        return;
    }

    // Discover ANCS service and characteristics.
    g_ancs_svc = g_client->getService(ANCS_SVC_UUID);
    if (!g_ancs_svc) {
        LOG("[ancs] ANCS service not found — is Bluetooth permission granted?\n");
        return;
    }

    g_ns_char = g_ancs_svc->getCharacteristic(ANCS_NS_UUID);
    g_cp_char = g_ancs_svc->getCharacteristic(ANCS_CP_UUID);
    g_ds_char = g_ancs_svc->getCharacteristic(ANCS_DS_UUID);

    if (!g_ns_char || !g_cp_char || !g_ds_char) {
        LOG("[ancs] missing NS/CP/DS characteristics\n");
        return;
    }

    // Subscribe to Notification Source.
    if (g_ns_char->canNotify()) {
        g_ns_char->subscribe(true, notify_ns_cb, false);
        LOG("[ancs] subscribed to NS\n");
    }

    // Subscribe to Data Source.
    if (g_ds_char->canNotify()) {
        g_ds_char->subscribe(true, notify_ds_cb, false);
        LOG("[ancs] subscribed to DS\n");
    }

    state_machine::set_phone_connected(true);
}

void on_iphone_disconnected() {
    LOG("[ancs] iPhone disconnected\n");
    g_conn_handle = 0xFFFF;
    g_client      = nullptr;
    g_ancs_svc    = nullptr;
    g_ns_char     = nullptr;
    g_cp_char     = nullptr;
    g_ds_char     = nullptr;

    // Clear queue and update status bar.
    g_queue_count = 0;
    mock_data::AncsConfig cfg = {};
    cfg.phone_connected = false;
    cfg.count = 0;
    mock_data::set_ancs_config(cfg);

    state_machine::set_phone_connected(false);
}

void on_notification_source(const uint8_t* data, uint16_t len) {
    if (len < 8) return;

    uint8_t  event_id = data[0];
    // uint8_t  flags    = data[1];   // reserved for future filtering
    // uint8_t  cat_id   = data[2];   // reserved for future filtering
    uint32_t uid      = (uint32_t)(data[4] | (data[5] << 8) |
                                    (data[6] << 16) | (data[7] << 24));

    if (event_id == 0) {
        // Added — request attributes to get bundle ID.
        ancs_client::request_attributes(uid);
    } else if (event_id == 2) {
        // Removed.
        queue_remove(uid);
    }
    // event_id == 1 (Modified) — ignore for now.
}

void on_data_source(const uint8_t* data, uint16_t len) {
    if (len == 0) return;

    // Accumulate DS data.
    size_t copy_len = len;
    if (g_ds_len + copy_len > sizeof(g_ds_buf)) {
        copy_len = sizeof(g_ds_buf) - g_ds_len;
    }
    memcpy(g_ds_buf + g_ds_len, data, copy_len);
    g_ds_len += copy_len;

    // A GetNotificationAttributes response starts with:
    //   [0] CommandID = 0x00
    //   [1-4] NotificationUID (uint32 LE)
    //   then attribute records: [AttributeID][Length(LE)][Data]
    if (g_ds_len < 5) return;
    if (g_ds_buf[0] != 0x00) {
        g_ds_len = 0; // not a GetNotificationAttributes response
        return;
    }

    uint32_t resp_uid = (uint32_t)(g_ds_buf[1] | (g_ds_buf[2] << 8) |
                                    (g_ds_buf[3] << 16) | (g_ds_buf[4] << 24));

    // Parse attribute records.
    // We requested: AppIdentifier (0x00), Title (0x01), Message (0x03)
    char app_id[128] = {};
    char title[193]  = {};
    char body[513]   = {};
    bool got_app_id  = false;
    bool got_title   = false;
    bool got_body    = false;

    size_t pos = 5;
    while (pos + 3 <= g_ds_len) {
        uint8_t  attr_id   = g_ds_buf[pos];
        uint16_t attr_len  = (uint16_t)(g_ds_buf[pos + 1] | (g_ds_buf[pos + 2] << 8));
        pos += 3;
        if (pos + attr_len > g_ds_len) break; // incomplete — wait for more

        char* dst = nullptr;
        size_t dst_sz = 0;

        if (attr_id == 0x00) { dst = app_id;  dst_sz = sizeof(app_id)  - 1; got_app_id = true; }
        else if (attr_id == 0x01) { dst = title;  dst_sz = sizeof(title)  - 1; got_title  = true; }
        else if (attr_id == 0x03) { dst = body;   dst_sz = sizeof(body)   - 1; got_body   = true; }

        if (dst) {
            size_t copy = attr_len < dst_sz ? attr_len : dst_sz;
            memcpy(dst, &g_ds_buf[pos], copy);
            dst[copy] = '\0';
        }
        pos += attr_len;
    }

    // Only process once we have the app identifier.
    if (!got_app_id) return;

    // Resolve icon token.
    const char* resolved_token = resolve_token(app_id);
    const char* token = resolved_token ? resolved_token : "unknown";

    // Add to display queue.
    queue_add(resp_uid, token);

    // Store pending notification info for the detail modal.
    g_pending_info.uid = resp_uid;
    strncpy(g_pending_info.app_id,  app_id,  sizeof(g_pending_info.app_id)  - 1);
    strncpy(g_pending_info.title,   title,   sizeof(g_pending_info.title)   - 1);
    strncpy(g_pending_info.body,    body,    sizeof(g_pending_info.body)    - 1);
    g_pending_info.icon_token = token;
    g_pending_valid = true;

    // Reset DS buffer for next response.
    g_ds_len = 0;
    g_pending_uid = 0;

    LOG("[ancs] attr response: uid=%u app=%s title='%s'\n",
                   (unsigned)resp_uid, app_id, title);
}

void request_attributes(uint32_t notif_uid) {
    if (!g_cp_char) return;
    g_pending_uid = notif_uid;
    g_ds_len      = 0;

    // GetNotificationAttributes command:
    //   CommandID=0x00, UID(4B), AppID(0x00), Title(0x01,max_len=192), Message(0x03,max_len=512)
    uint8_t cmd[16];
    cmd[0] = 0x00; // GetNotificationAttributes
    cmd[1] = (uint8_t)(notif_uid & 0xFF);
    cmd[2] = (uint8_t)((notif_uid >> 8) & 0xFF);
    cmd[3] = (uint8_t)((notif_uid >> 16) & 0xFF);
    cmd[4] = (uint8_t)((notif_uid >> 24) & 0xFF);
    cmd[5]  = 0x00; // AppIdentifier attribute
    cmd[6]  = 0x01; // Title
    cmd[7]  = 192 & 0xFF;
    cmd[8]  = (192 >> 8) & 0xFF;
    cmd[9]  = 0x03; // Message
    cmd[10] = 512 & 0xFF;
    cmd[11] = (512 >> 8) & 0xFF;

    g_cp_char->writeValue(cmd, 12, true);
    LOG("[ancs] GetNotificationAttributes uid=%u\n", (unsigned)notif_uid);
}

void dismiss_notification(uint32_t notif_uid) {
    // Send PerformNotificationAction(Negative) to clear it on the phone.
    if (g_cp_char) {
        uint8_t cmd[6];
        cmd[0] = 0x02; // PerformNotificationAction
        cmd[1] = (uint8_t)(notif_uid & 0xFF);
        cmd[2] = (uint8_t)((notif_uid >> 8) & 0xFF);
        cmd[3] = (uint8_t)((notif_uid >> 16) & 0xFF);
        cmd[4] = (uint8_t)((notif_uid >> 24) & 0xFF);
        cmd[5] = 0x01; // ActionID Negative
        g_cp_char->writeValue(cmd, 6, true);
    }
    queue_remove(notif_uid);
}

const NotificationInfo* pending_notification_info() {
    return g_pending_valid ? &g_pending_info : nullptr;
}

void clear_pending_notification_info() {
    g_pending_valid = false;
}

const QueueEntry* get_queue(size_t* count_out) {
    if (count_out) *count_out = g_queue_count;
    return g_queue;
}

} // namespace ancs_client

// ── NS / DS notify callbacks (NimBLE task context) ────────────────────────

namespace {

static void notify_ns_cb(NimBLERemoteCharacteristic* /*c*/,
                          uint8_t* data, size_t len, bool /*is_notify*/) {
    ancs_client::on_notification_source(data, (uint16_t)len);
}

static void notify_ds_cb(NimBLERemoteCharacteristic* /*c*/,
                          uint8_t* data, size_t len, bool /*is_notify*/) {
    ancs_client::on_data_source(data, (uint16_t)len);
}

} // namespace
