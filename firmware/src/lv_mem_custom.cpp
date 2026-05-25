#include "lv_mem_custom.h"
#include <esp_heap_caps.h>

// Routes LVGL's internal heap to PSRAM so the 64 KB internal SRAM limit
// never starves the UI object tree. Draw buffers are statically declared
// in lvgl_display.cpp (SRAM, DMA-safe) and do not go through this allocator.

extern "C" {

void* lv_psram_malloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(size);  // fallback to SRAM if PSRAM saturated
    return p;
}

void* lv_psram_realloc(void* ptr, size_t size) {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_psram_free(void* ptr) {
    heap_caps_free(ptr);
}

}
