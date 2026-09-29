/*
 * lv_mem_psram.c - LVGL heap in PSRAM (LV_USE_CUSTOM_MALLOC).
 *
 * With the C-library allocator every LVGL object (< SPIRAM_MALLOC_ALWAYSINTERNAL
 * = 16 KB) landed in internal RAM first.  Ten pages of widgets consumed all of
 * it (boot log: "internal 0 KB"), leaving the Wi-Fi driver without RX/TX
 * buffers - esp_wifi_init() then aborted with ESP_ERR_NO_MEM and the board
 * reboot-looped (seen as endless screen flashes).  The UI tree lives in PSRAM
 * instead (64 KB / 64 B data cache keeps traversal fast); internal RAM stays
 * for Wi-Fi, lwIP, DMA and task stacks.  Internal RAM is only a fallback.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#define LV_CAPS_PREF  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_CAPS_FALL  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, LV_CAPS_PREF, LV_CAPS_FALL);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc_prefer(p, new_size, 2, LV_CAPS_PREF, LV_CAPS_FALL);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    memset(mon_p, 0, sizeof(*mon_p));
    mon_p->total_size = heap_caps_get_total_size(LV_CAPS_PREF);
    mon_p->free_size = heap_caps_get_free_size(LV_CAPS_PREF);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(LV_CAPS_PREF);
    mon_p->used_pct = mon_p->total_size ?
        (uint8_t)(100 - mon_p->free_size * 100 / mon_p->total_size) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity_all(true) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
