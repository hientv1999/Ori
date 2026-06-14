// Ori — profile photo + PTO destination image decode-once PSRAM cache.
//
// Profile photo: 228×228 JPEG (≤40 KB). PTO image: 528×396 JPEG (≤64 KB).
// Raw JPEG persisted to LittleFS (/photos/ directory); decoded RGB565 held in
// PSRAM. LVGL renders directly from PSRAM — zero re-decode per frame.
//
// LittleFS must be mounted (LittleFS.begin()) before any function in this
// module is called. Call photo_cache::mount_fs() from main setup() first.
//
// Empty image (len == 0 in store_pto) means the user set no destination
// photo in Orion; get_pto() returns nullptr and screen_pto uses the placeholder.

#include "photo_cache.h"

#include <Arduino.h>
#include "ori_log.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "libs/tjpgd/tjpgd.h"
#include "widgets/widget_profile_card.h"

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint16_t PHOTO_W = 228;
static constexpr uint16_t PHOTO_H = 228;

static constexpr uint16_t PTO_W = 528;
static constexpr uint16_t PTO_H = 396;

static constexpr size_t PROFILE_MAX_JPEG = 200 * 1024;
static constexpr size_t PTO_MAX_JPEG     = 512 * 1024;

static constexpr const char* PATH_PROFILE = "/photos/profile.jpg";
static constexpr const char* PATH_PTO     = "/photos/pto.jpg";

// ── Shared tjpgd decode plumbing ──────────────────────────────────────────────

namespace {

struct DecodeCtx {
    const uint8_t* jpeg;
    size_t         pos;
    size_t         len;
    uint16_t*      out_buf;
    uint16_t       out_w;
    uint16_t       out_h;
};

static DecodeCtx g_dec;

static size_t jpeg_read_cb(JDEC* jd, uint8_t* buf, size_t nb) {
    (void)jd;
    size_t avail = g_dec.len - g_dec.pos;
    if (nb > avail) nb = avail;
    if (buf) memcpy(buf, g_dec.jpeg + g_dec.pos, nb);
    g_dec.pos += nb;
    return nb;
}

static int jpeg_write_cb(JDEC* jd, void* bitmap, JRECT* rect) {
    (void)jd;
    if (!g_dec.out_buf) return 0;
    const uint8_t* src = static_cast<const uint8_t*>(bitmap);
    uint16_t bw = (uint16_t)(rect->right  - rect->left + 1);
    uint16_t bh = (uint16_t)(rect->bottom - rect->top  + 1);
    for (uint16_t row = 0; row < bh; ++row) {
        uint16_t y = rect->top + row;
        if (y >= g_dec.out_h) break;
        for (uint16_t col = 0; col < bw; ++col) {
            uint16_t x = rect->left + col;
            if (x >= g_dec.out_w) { src += 3; continue; }
            uint8_t b = *src++, g = *src++, r = *src++;  // tjpgd JD_FORMAT=0 outputs BGR, not RGB
            g_dec.out_buf[y * g_dec.out_w + x] =
                (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
        }
    }
    return 1;
}

static bool decode_jpeg_to(const uint8_t* jpeg, size_t len,
                            uint16_t* out_buf, uint16_t w, uint16_t h) {
    static uint8_t work[4096];
    g_dec = { jpeg, 0, len, out_buf, w, h };
    JDEC jd = {};
    JRESULT res = jd_prepare(&jd, jpeg_read_cb, work, sizeof(work), nullptr);
    if (res != JDR_OK) {
        LOG("[photo_cache] jd_prepare failed: %d\n", (int)res);
        return false;
    }
    if (jd.width > w || jd.height > h) {
        LOG("[photo_cache] JPEG too large: %ux%u (max %ux%u)\n",
                      (unsigned)jd.width, (unsigned)jd.height, (unsigned)w, (unsigned)h);
        return false;
    }
    res = jd_decomp(&jd, jpeg_write_cb, 0);
    if (res != JDR_OK) {
        LOG("[photo_cache] jd_decomp failed: %d\n", (int)res);
        return false;
    }
    return true;
}

static void build_dsc(lv_image_dsc_t* dsc, const uint16_t* buf, uint16_t w, uint16_t h) {
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags  = 0;
    dsc->header.w      = w;
    dsc->header.h      = h;
    dsc->header.stride = (uint32_t)w * 2;
    dsc->data_size     = (uint32_t)w * h * 2;
    dsc->data          = reinterpret_cast<const uint8_t*>(buf);
}

// ── LittleFS helpers ──────────────────────────────────────────────────────────

// Load JPEG from LittleFS path, decode to a new PSRAM buffer. Returns buffer or nullptr.
static uint16_t* load_and_decode(const char* path, size_t max_jpeg_bytes,
                                  uint16_t w, uint16_t h) {
    File f = LittleFS.open(path, "r");
    if (!f) return nullptr;

    size_t sz = f.size();
    if (sz == 0 || sz > max_jpeg_bytes) {
        f.close();
        return nullptr;
    }

    // PSRAM-only: never fall back to internal DRAM. A freed DRAM JPEG buffer
    // leaves JFIF SOI bytes (0xFF D8 FF E0) in the heap; if a subsequent NVS
    // allocation lands there, the JPEG bytes corrupt the FreeRTOS Queue_t
    // pxQueueSetContainer field → LoadProhibited in prvNotifyQueueSetContainer.
    // At boot, PSRAM is nearly empty, so ≤512 KB always fits.
    uint8_t* jpeg_buf = static_cast<uint8_t*>(
        heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jpeg_buf) { f.close(); return nullptr; }

    size_t got = f.read(jpeg_buf, sz);
    f.close();

    if (got != sz) {
        LOG("[photo_cache] LittleFS read mismatch: got=%u want=%u\n",
                      (unsigned)got, (unsigned)sz);
        free(jpeg_buf);
        return nullptr;
    }

    uint16_t* rgb_buf = static_cast<uint16_t*>(
        heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM));
    if (!rgb_buf) {
        LOG("[photo_cache] PSRAM alloc failed\n");
        free(jpeg_buf);
        return nullptr;
    }

    bool ok = decode_jpeg_to(jpeg_buf, sz, rgb_buf, w, h);
    free(jpeg_buf);

    if (!ok) { heap_caps_free(rgb_buf); return nullptr; }
    return rgb_buf;
}

// Save JPEG to LittleFS path, decode to a new PSRAM buffer. Returns buffer or nullptr.
static uint16_t* save_and_decode(const char* path, size_t max_jpeg_bytes,
                                  const uint8_t* jpeg, size_t len,
                                  uint16_t w, uint16_t h) {
    if (len > max_jpeg_bytes) return nullptr;

    // Ensure /photos directory exists.
    if (!LittleFS.exists("/photos")) LittleFS.mkdir("/photos");

    File f = LittleFS.open(path, "w");
    if (f) {
        f.write(jpeg, len);
        f.close();
    } else {
        LOG("[photo_cache] LittleFS open failed: %s\n", path);
    }

    uint16_t* rgb_buf = static_cast<uint16_t*>(
        heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM));
    if (!rgb_buf) {
        LOG("[photo_cache] PSRAM alloc failed\n");
        return nullptr;
    }
    if (!decode_jpeg_to(jpeg, len, rgb_buf, w, h)) {
        heap_caps_free(rgb_buf);
        return nullptr;
    }
    return rgb_buf;
}

static void erase_file(const char* path) {
    if (LittleFS.exists(path)) {
        LittleFS.remove(path);
    }
}

// ── Photo state ───────────────────────────────────────────────────────────────

uint16_t*      g_profile_buf      = nullptr;
lv_image_dsc_t g_profile_dsc      = {};
bool           g_profile_ready    = false;

uint16_t*      g_profile_ph_buf   = nullptr;
lv_image_dsc_t g_profile_ph_dsc   = {};
bool           g_profile_ph_ready  = false;

uint16_t*      g_pto_buf          = nullptr;
lv_image_dsc_t g_pto_dsc          = {};
bool           g_pto_ready         = false;

uint16_t*      g_pto_ph_buf       = nullptr;
lv_image_dsc_t g_pto_ph_dsc       = {};
bool           g_pto_ph_ready      = false;

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

namespace photo_cache {

void mount_fs() {
    if (!LittleFS.begin(/*formatOnFail=*/true)) {
        LOG("[photo_cache] LittleFS mount failed\n");
    } else {
        LOG("[photo_cache] LittleFS mounted (%.1f KB free)\n",
                      (float)LittleFS.totalBytes() / 1024.0f);
    }
}

// ── Profile photo ─────────────────────────────────────────────────────────────

void init() {
    uint16_t* buf = load_and_decode(PATH_PROFILE, PROFILE_MAX_JPEG, PHOTO_W, PHOTO_H);
    if (buf) {
        if (g_profile_buf) heap_caps_free(g_profile_buf);
        g_profile_buf   = buf;
        g_profile_ready = true;
        build_dsc(&g_profile_dsc, g_profile_buf, PHOTO_W, PHOTO_H);
        widget_profile_card::set_photo(&g_profile_dsc);
        LOG("[photo_cache] profile photo loaded from LittleFS\n");
    } else {
        widget_profile_card::set_photo(get_profile_placeholder());
    }
}

void store(uint8_t* jpeg, size_t len) {
    if (len == 0) {
        if (jpeg) heap_caps_free(jpeg);
        LOG("[photo_cache] store: empty — clearing profile photo\n");
        clear();
        return;
    }
    if (!jpeg || len > PROFILE_MAX_JPEG) {
        if (jpeg) heap_caps_free(jpeg);
        LOG("[photo_cache] store: invalid (len=%u)\n", (unsigned)len);
        return;
    }
    uint16_t* buf = save_and_decode(PATH_PROFILE, PROFILE_MAX_JPEG,
                                     jpeg, len, PHOTO_W, PHOTO_H);
    heap_caps_free(jpeg);
    if (buf) {
        if (g_profile_buf) heap_caps_free(g_profile_buf);
        g_profile_buf   = buf;
        g_profile_ready = true;
        build_dsc(&g_profile_dsc, g_profile_buf, PHOTO_W, PHOTO_H);
        LOG("[photo_cache] profile photo stored to LittleFS (%u bytes)\n", (unsigned)len);
        widget_profile_card::set_photo(&g_profile_dsc);
    } else {
        LOG("[photo_cache] profile photo decode failed\n");
    }
}

const lv_image_dsc_t* get() {
    return g_profile_ready ? &g_profile_dsc : nullptr;
}

void clear() {
    g_profile_ready = false;
    erase_file(PATH_PROFILE);
    if (g_profile_buf) { heap_caps_free(g_profile_buf); g_profile_buf = nullptr; }
    g_profile_dsc = {};
    widget_profile_card::set_photo(get_profile_placeholder());
    LOG("[photo_cache] profile photo cleared\n");
}

void init_profile_placeholder(const uint8_t* jpeg, size_t len) {
    if (!jpeg || !len) return;
    uint16_t* buf = static_cast<uint16_t*>(
        heap_caps_malloc((size_t)PHOTO_W * PHOTO_H * 2, MALLOC_CAP_SPIRAM));
    if (!buf) { LOG("[photo_cache] profile placeholder PSRAM alloc failed\n"); return; }
    if (!decode_jpeg_to(jpeg, len, buf, PHOTO_W, PHOTO_H)) {
        heap_caps_free(buf);
        LOG("[photo_cache] profile placeholder decode failed\n");
        return;
    }
    if (g_profile_ph_buf) heap_caps_free(g_profile_ph_buf);
    g_profile_ph_buf   = buf;
    g_profile_ph_ready = true;
    build_dsc(&g_profile_ph_dsc, g_profile_ph_buf, PHOTO_W, PHOTO_H);
    LOG("[photo_cache] profile placeholder ready (%u bytes)\n", (unsigned)len);
}

const lv_image_dsc_t* get_profile_placeholder() {
    return g_profile_ph_ready ? &g_profile_ph_dsc : nullptr;
}

// ── PTO image ─────────────────────────────────────────────────────────────────

void init_pto() {
    uint16_t* buf = load_and_decode(PATH_PTO, PTO_MAX_JPEG, PTO_W, PTO_H);
    if (buf) {
        if (g_pto_buf) heap_caps_free(g_pto_buf);
        g_pto_buf   = buf;
        g_pto_ready = true;
        build_dsc(&g_pto_dsc, g_pto_buf, PTO_W, PTO_H);
        LOG("[photo_cache] PTO image loaded from LittleFS\n");
    }
}

void store_pto(uint8_t* jpeg, size_t len) {
    if (len == 0) {
        if (jpeg) heap_caps_free(jpeg);
        clear_pto();
        return;
    }
    if (len > PTO_MAX_JPEG) {
        LOG("[photo_cache] store_pto: too large (%u bytes)\n", (unsigned)len);
        if (jpeg) heap_caps_free(jpeg);
        return;
    }
    uint16_t* buf = save_and_decode(PATH_PTO, PTO_MAX_JPEG,
                                     jpeg, len, PTO_W, PTO_H);
    heap_caps_free(jpeg);
    if (buf) {
        if (g_pto_buf) heap_caps_free(g_pto_buf);
        g_pto_buf   = buf;
        g_pto_ready = true;
        build_dsc(&g_pto_dsc, g_pto_buf, PTO_W, PTO_H);
        LOG("[photo_cache] PTO image stored to LittleFS (%u bytes)\n", (unsigned)len);
    } else {
        LOG("[photo_cache] PTO image decode failed\n");
    }
}

const lv_image_dsc_t* get_pto() {
    return g_pto_ready ? &g_pto_dsc : nullptr;
}

void clear_pto() {
    g_pto_ready = false;
    erase_file(PATH_PTO);
    if (g_pto_buf) { heap_caps_free(g_pto_buf); g_pto_buf = nullptr; }
    g_pto_dsc = {};
    LOG("[photo_cache] PTO image cleared\n");
}

void init_pto_placeholder(const uint8_t* jpeg, size_t len) {
    if (!jpeg || !len) return;
    uint16_t* buf = static_cast<uint16_t*>(
        heap_caps_malloc((size_t)PTO_W * PTO_H * 2, MALLOC_CAP_SPIRAM));
    if (!buf) { LOG("[photo_cache] PTO placeholder PSRAM alloc failed\n"); return; }
    if (!decode_jpeg_to(jpeg, len, buf, PTO_W, PTO_H)) {
        heap_caps_free(buf);
        LOG("[photo_cache] PTO placeholder decode failed\n");
        return;
    }
    if (g_pto_ph_buf) heap_caps_free(g_pto_ph_buf);
    g_pto_ph_buf   = buf;
    g_pto_ph_ready = true;
    build_dsc(&g_pto_ph_dsc, g_pto_ph_buf, PTO_W, PTO_H);
    LOG("[photo_cache] PTO placeholder ready (%u bytes)\n", (unsigned)len);
}

const lv_image_dsc_t* get_pto_placeholder() {
    return g_pto_ph_ready ? &g_pto_ph_dsc : nullptr;
}

} // namespace photo_cache
