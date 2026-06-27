// Ori NVS Sync Data — M5
//
// SHA-256 hashes and profile strings for the hash-manifest delta reconnect.
// All operations use the same "ori" Preferences namespace as nvs_store.cpp.

#include "nvs_sync.h"

#include <Arduino.h>
#include <Preferences.h>

namespace nvs_sync {

// ── Hash key constants ────────────────────────────────────────────────────────

const char* const HASH_KEY_PROFILE   = "p_sha";
const char* const HASH_KEY_PHOTO     = "ph_sha";
const char* const HASH_KEY_MEETINGS  = "m_sha";
const char* const HASH_KEY_PTO       = "pto_sha";

namespace {
constexpr const char* NS = "ori";

// Key names for profile strings and metadata.
constexpr const char* K_NAME   = "p_name";
constexpr const char* K_TITLE  = "p_titl";
constexpr const char* K_EMAIL  = "p_email";
constexpr const char* K_PHONE  = "p_phone";
constexpr const char* K_EPOCH  = "epoch";

Preferences prefs;

// PTO metadata RAM cache.
//
// ESP-IDF's NVS handle allocator uses SRAM heap. After the media screen
// allocates and frees LVGL objects, the heap layout can place a new
// NVSHandleSimple at an address whose vtable field overlaps a prior JFIF
// buffer, producing a LoadProhibited crash inside load_pto_meta().
//
// Fix: populate once at startup (prime_pto_cache(), clean heap) so that
// all later calls — including from screen_pto::create() — read from RAM.
struct PtoCache {
    uint32_t start;
    uint32_t end;
    char     dest[129];
    bool     loaded;
};
static PtoCache g_pto_cache = {};

static void load_pto_from_nvs() {
    g_pto_cache = {};       // zero fields
    g_pto_cache.loaded = true;  // mark done even if NVS fails
    if (!prefs.begin(NS, /*readOnly=*/true)) return;
    g_pto_cache.start = prefs.getUInt("pto_s", 0);
    g_pto_cache.end   = prefs.getUInt("pto_e", 0);
    String d = prefs.getString("pto_d", "");
    prefs.end();
    size_t dlen = d.length();
    if (dlen >= sizeof(g_pto_cache.dest)) dlen = sizeof(g_pto_cache.dest) - 1;
    memcpy(g_pto_cache.dest, d.c_str(), dlen);
    g_pto_cache.dest[dlen] = '\0';
}

// Sync-hash RAM cache — same rationale as PtoCache above. handle_manifest_write()
// (gatt_server.cpp) calls load_hash() up to 4 times per reconnect, on the NimBLE
// host task, right after a sync session that may have decoded/freed JPEG buffers.
// A fresh nvs_open() there can land on heap previously holding JFIF bytes and
// corrupt the NVS lock semaphore's Queue_t (LoadProhibited in
// prvNotifyQueueSetContainer, A7/EXCVADDR = 0xe0ffd8ff-ish — "FF D8 FF E0").
// Fix: prime once at startup (clean heap) and keep in sync via save_hash();
// load_hash() never calls nvs_open().
struct HashCache {
    uint8_t profile[32];
    uint8_t photo[32];
    uint8_t pto[32];
    bool has_profile;
    bool has_photo;
    bool has_pto;
    bool loaded;
};
static HashCache g_hash_cache = {};

static void load_one_hash(const char* key, uint8_t out[32], bool* has) {
    size_t sz = prefs.getBytesLength(key);
    if (sz == 32) {
        prefs.getBytes(key, out, 32);
        *has = true;
    }
}

static void load_hashes_from_nvs() {
    g_hash_cache = {};
    g_hash_cache.loaded = true; // mark done even if NVS fails
    if (!prefs.begin(NS, /*readOnly=*/true)) return;
    load_one_hash(HASH_KEY_PROFILE, g_hash_cache.profile, &g_hash_cache.has_profile);
    load_one_hash(HASH_KEY_PHOTO,   g_hash_cache.photo,   &g_hash_cache.has_photo);
    load_one_hash(HASH_KEY_PTO,     g_hash_cache.pto,     &g_hash_cache.has_pto);
    prefs.end();
}

} // namespace

// ── SHA-256 hash storage ───────────────────────────────────────────────────────

void prime_hash_cache() {
    if (!g_hash_cache.loaded) load_hashes_from_nvs();
}

bool load_hash(const char* key, uint8_t out_hash[32]) {
    if (!g_hash_cache.loaded) load_hashes_from_nvs();
    if (key == HASH_KEY_PROFILE) {
        if (!g_hash_cache.has_profile) return false;
        memcpy(out_hash, g_hash_cache.profile, 32);
        return true;
    }
    if (key == HASH_KEY_PHOTO) {
        if (!g_hash_cache.has_photo) return false;
        memcpy(out_hash, g_hash_cache.photo, 32);
        return true;
    }
    if (key == HASH_KEY_PTO) {
        if (!g_hash_cache.has_pto) return false;
        memcpy(out_hash, g_hash_cache.pto, 32);
        return true;
    }
    // HASH_KEY_MEETINGS: meetings are RAM-only (not NVS-persisted) — the
    // manifest handler compares against its own RAM hash, not via load_hash().
    // Shortcut Config has no hash at all — see ble-protocol.md §6.0.
    return false;
}

void save_hash(const char* key, const uint8_t hash[32]) {
    if (prefs.begin(NS, /*readOnly=*/false)) {
        prefs.putBytes(key, hash, 32);
        prefs.end();
    }
    // Keep the RAM cache in sync so load_hash() reflects this write immediately.
    if (!g_hash_cache.loaded) load_hashes_from_nvs();
    if (key == HASH_KEY_PROFILE) {
        memcpy(g_hash_cache.profile, hash, 32);
        g_hash_cache.has_profile = true;
    } else if (key == HASH_KEY_PHOTO) {
        memcpy(g_hash_cache.photo, hash, 32);
        g_hash_cache.has_photo = true;
    } else if (key == HASH_KEY_PTO) {
        memcpy(g_hash_cache.pto, hash, 32);
        g_hash_cache.has_pto = true;
    }
}

// ── Profile string fields ─────────────────────────────────────────────────────

void save_profile(const char* name, const char* title,
                  const char* email, const char* phone) {
    if (!prefs.begin(NS, /*readOnly=*/false)) return;
    if (name)  prefs.putString(K_NAME,  name);
    if (title) prefs.putString(K_TITLE, title);
    if (email) prefs.putString(K_EMAIL, email);
    if (phone) prefs.putString(K_PHONE, phone);
    prefs.end();
}

bool load_profile(char* out_name,  size_t name_sz,
                  char* out_title, size_t title_sz,
                  char* out_email, size_t email_sz,
                  char* out_phone, size_t phone_sz) {
    if (!prefs.begin(NS, /*readOnly=*/true)) return false;

    String n = prefs.getString(K_NAME,  "");
    String t = prefs.getString(K_TITLE, "");
    String e = prefs.getString(K_EMAIL, "");
    String p = prefs.getString(K_PHONE, "");
    prefs.end();

    if (out_name  && name_sz  > 0) strncpy(out_name,  n.c_str(), name_sz  - 1);
    if (out_title && title_sz > 0) strncpy(out_title, t.c_str(), title_sz - 1);
    if (out_email && email_sz > 0) strncpy(out_email, e.c_str(), email_sz - 1);
    if (out_phone && phone_sz > 0) strncpy(out_phone, p.c_str(), phone_sz - 1);

    return n.length() > 0;
}

// ── Last synced epoch ─────────────────────────────────────────────────────────

void save_epoch(uint32_t epoch_utc) {
    if (prefs.begin(NS, /*readOnly=*/false)) {
        prefs.putUInt(K_EPOCH, epoch_utc);
        prefs.end();
    }
}

uint32_t load_epoch() {
    uint32_t v = 0;
    if (prefs.begin(NS, /*readOnly=*/true)) {
        v = prefs.getUInt(K_EPOCH, 0);
        prefs.end();
    }
    return v;
}

// ── PTO metadata ──────────────────────────────────────────────────────────────

void prime_pto_cache() {
    if (!g_pto_cache.loaded) load_pto_from_nvs();
}

void save_pto_meta(uint32_t start, uint32_t end, const char* destination) {
    if (!prefs.begin(NS, /*readOnly=*/false)) return;
    prefs.putUInt("pto_s",  start);
    prefs.putUInt("pto_e",  end);
    if (destination) prefs.putString("pto_d", destination);
    prefs.end();
    // Keep cache in sync so load_pto_meta() doesn't re-read NVS.
    g_pto_cache.start = start;
    g_pto_cache.end   = end;
    if (destination) {
        strncpy(g_pto_cache.dest, destination, sizeof(g_pto_cache.dest) - 1);
        g_pto_cache.dest[sizeof(g_pto_cache.dest) - 1] = '\0';
    } else {
        g_pto_cache.dest[0] = '\0';
    }
    g_pto_cache.loaded = true;
}

bool load_pto_meta(uint32_t* out_start, uint32_t* out_end,
                   char* out_dest, size_t dest_sz) {
    if (!g_pto_cache.loaded) load_pto_from_nvs();
    if (out_start) *out_start = g_pto_cache.start;
    if (out_end)   *out_end   = g_pto_cache.end;
    if (out_dest && dest_sz > 0) {
        strncpy(out_dest, g_pto_cache.dest, dest_sz - 1);
        out_dest[dest_sz - 1] = '\0';
    }
    return g_pto_cache.start != 0;
}

// ── Meeting list CBOR blob ─────────────────────────────────────────────────────

bool save_meetings_blob(const uint8_t* buf, size_t len) {
    if (!buf || !len) return false;
    if (!prefs.begin(NS, /*readOnly=*/false)) return false;
    size_t written = prefs.putBytes("m_cbor", buf, len);
    prefs.end();
    return written == len;
}

size_t load_meetings_blob(uint8_t* out, size_t max_len) {
    if (!out || !max_len) return 0;
    if (!prefs.begin(NS, /*readOnly=*/true)) return 0;
    size_t sz = prefs.getBytesLength("m_cbor");
    if (!sz || sz > max_len) { prefs.end(); return 0; }
    prefs.getBytes("m_cbor", out, sz);
    prefs.end();
    return sz;
}

} // namespace nvs_sync
