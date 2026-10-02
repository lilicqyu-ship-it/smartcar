/*
 * app_main.c - boot orchestration / composition root (mirrors esp32c6_car main).
 *
 * Order (spec 81: fast to Ready, spec 6: P0 boot page):
 *   NVS -> state/settings -> LCD+touch (BSP) -> UI (P0 boot page)
 *   -> radio link task -> control task -> mark boot complete
 * The UI switches from P0 to the drive page on its own once all stages are
 * marked; nothing here blocks on the radio.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "bsp/esp-bsp.h"

#include "app_state.h"
#include "scr_settings.h"
#include "scr_link.h"
#include "scr_ctrl.h"
#include "scr_cam.h"
#include "scr_svc.h"
#include "esp_heap_caps.h"
#include "ui/ui.h"

static const char *TAG = "scr_main";

/* GT1151 release debounce.  The board has no touch INT line, so LVGL polls the
 * controller every 10 ms; the driver clears the report register after each
 * read, and a poll that lands before the chip's next scan reads "0 points".
 * A resting finger therefore produced spurious RELEASED -> PRESSED pairs, and
 * every one zeroed the stick (THR/STR flashed to 0).  Hold the last pressed
 * point until the controller has reported no touch for TOUCH_RELEASE_MS. */
#define TOUCH_RELEASE_MS 60
static lv_indev_read_cb_t s_touch_read;

static void touch_read_debounced(lv_indev_t *indev, lv_indev_data_t *data)
{
    static lv_point_t last_pt;
    static uint32_t   last_ms;
    static bool       held;

    s_touch_read(indev, data);
    if (data->state == LV_INDEV_STATE_PRESSED) {
        last_pt = data->point;
        last_ms = lv_tick_get();
        held = true;
    } else if (held && lv_tick_elaps(last_ms) < TOUCH_RELEASE_MS) {
        data->state = LV_INDEV_STATE_PRESSED;   /* gap between scans, not a lift */
        data->point = last_pt;
    } else {
        held = false;
    }
}

void app_main(void)
{
    /* NVS first: settings and the pairing token live there */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_state_init();
    scr_settings_init();

    scr_settings_t set;
    scr_settings_get(&set);
    app_state_set_mode((scr_mode_t)set.mode);

    /* LCD + touch + LVGL task, all-in-one from the BSP (spec 77: use the
     * existing board framework, no second GUI stack).  Larger LVGL stack:
     * every page callback (settings keyboard included) runs on it.  Pinned
     * to core 1: Wi-Fi/lwip dominate core 0, the 30 Hz ctrl task shares
     * core 1 with LVGL at higher prio (microsecond bursts every 33 ms). */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 9216;
    port_cfg.task_affinity = 1;
    bsp_display_cfg_t bsp_cfg = {
        .lvgl_port_cfg = port_cfg,
    };
    lv_display_t *disp = bsp_display_start_with_config(&bsp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }
    app_state_boot_mark_lcd();
    app_state_boot_mark_touch();

    /* Touch at 100 Hz: the GT1151 has no interrupt line on this board, so
     * LVGL polls it on the indev timer, which defaults to the 33 ms refresh
     * period - the stick moved in 30 Hz steps (a phone samples at 60-120 Hz).
     * 10 ms is one short I2C read; the ctrl task still coalesces to 50 Hz. */
    lv_indev_t *touch = bsp_display_get_input_dev();
    if (touch && lv_indev_get_read_timer(touch)) {
        bsp_display_lock(0);
        lv_timer_set_period(lv_indev_get_read_timer(touch), 10);
        s_touch_read = lv_indev_get_read_cb(touch);
        if (s_touch_read) {
            lv_indev_set_read_cb(touch, touch_read_debounced);
        }
        bsp_display_unlock();
    }

    /* radio + control FIRST (spec 74/95.8/95.9): the Wi-Fi driver needs ~50 KB
     * of internal RAM at init.  Building the UI first let LVGL's small objects
     * take that RAM (all pages together) and esp_wifi_init() failed with
     * ESP_ERR_NO_MEM -> abort -> reboot loop (seen as endless screen flashes).
     * Started before the UI, the radio gets internal RAM and the UI spills to
     * PSRAM once internal RAM runs out (SPIRAM_USE_MALLOC). */
#if !CONFIG_SCR_BENCH_DISP_ONLY
    scr_link_start();
    scr_ctrl_start();
    scr_svc_start();        /* core 0: OTA / diag / calibration (doc/08) */
    scr_cam_start();        /* core 0: Camera WS video plane (doc 08 §2) */
#endif

    /* UI runs in the LVGL task context: take the display lock while building */
    bsp_display_lock(0);
    ui_init();
    bsp_display_unlock();

    ESP_LOGI(TAG, "heap after init: internal %u KB (min block %u KB), psram %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

#if CONFIG_SCR_BENCH_DISP_ONLY
    /* display bring-up experiment: P0 needs all four marks to advance */
    app_state_boot_mark_radio();
    app_state_log(SCR_LOG_INFO, "Bench display-only mode");
#endif

    app_state_boot_mark_sys();
    app_state_log(SCR_LOG_INFO, "Boot complete");
}
