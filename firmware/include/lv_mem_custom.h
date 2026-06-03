#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Core allocator hooks — macro-aliased by lv_conf.h to lv_malloc_core etc.
void* lv_psram_malloc(size_t size);
void* lv_psram_realloc(void* ptr, size_t size);
void  lv_psram_free(void* ptr);

// LVGL 9 LV_STDLIB_CUSTOM required symbols (see lv_mem.h).
// Declared here so lv_conf.h can include this header during LVGL compilation.
void          lv_mem_init(void);
void          lv_mem_deinit(void);

#ifdef __cplusplus
}
#endif
