#include "lv_mem_custom.h"
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <string.h>

// Routes LVGL's internal heap to PSRAM so the 64 KB internal SRAM limit
// never starves the UI object tree. Draw buffers are statically declared
// in lvgl_display.cpp (SRAM, DMA-safe) and do not go through this allocator.
//
// LVGL 9 LV_STDLIB_CUSTOM requires the user to supply the full memory
// management surface: init/deinit, add/remove pool, monitor, test, and the
// three allocator hooks (lv_malloc_core / lv_realloc_core / lv_free_core).
// The hooks are macro-aliased in lv_conf.h; the remaining functions are
// provided below as real symbols.

extern "C" {

// ─── Core allocator hooks (macro-aliased in lv_conf.h) ────────────────────

void* lv_psram_malloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(size);  // fallback to SRAM if PSRAM saturated
    return p;
}

void* lv_psram_realloc(void* ptr, size_t size) {
    void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p && size > 0) p = realloc(ptr, size);  // fallback to SRAM
    return p;
}

void lv_psram_free(void* ptr) {
    heap_caps_free(ptr);
}

// ─── Required LVGL 9 LV_STDLIB_CUSTOM symbols ─────────────────────────────

void lv_mem_init(void) {
    // PSRAM heap is initialised by esp-idf before app_main(); nothing to do.
}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* /*mem*/, size_t /*bytes*/) {
    return nullptr;  // dynamic pool extension not needed
}

void lv_mem_remove_pool(lv_mem_pool_t /*pool*/) {}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
    if (mon_p) memset(mon_p, 0, sizeof(*mon_p));
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}

} // extern "C"
