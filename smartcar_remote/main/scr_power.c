/*
 * scr_power.c - auto power-off: touch inactivity -> warn countdown -> deep sleep
 *
 * Idle clock: lv_display_get_inactive_time() - LVGL already timestamps the
 * last press on every input device (the debounced reader in app_main feeds
 * it), so the watchdog needs no second bookkeeping.  Tick sequence:
 *
 *   idle < timeout - warn      -> quiet (state machine parked)
 *   idle >= timeout - warn     -> full-screen veil, live countdown in seconds
 *   any touch                  -> activity resets, veil hides, clock restarts
 *   idle >= timeout            -> best-effort vehicle stop, backlight off,
 *                                 deep sleep (BOOT key = power on)
 *
 * Two deliberate suppressions:
 *   - OTA transfer (scr_svc): VERIFY/SEND/WAIT_TC is minutes of no-touch by
 *     design - cutting power mid-flash would brick the update window.
 *   - BENCH build (gated in app_main): the display bring-up bench must not
 *     power itself off mid jitter test.
 *
 * The veil is CLICKABLE on purpose: the cancel tap lands on the veil instead
 * of punching through to whatever page sits underneath (STOP included).
 * While it is up it also covers the ui_alert overlay for at most WARN_S -
 * radio loss during the countdown still stops the car on the C6 side, and
 * the alert re-surfaces the moment the user touches (cancel) or sleep wins.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "app_state.h"
#include "scr_ctrl.h"
#include "scr_svc.h"
#include "scr_power.h"
#include "ui_theme.h"

#if CONFIG_SCR_AUTO_OFF_ENABLE

static const char *TAG = "scr_power";

#define TICK_MS     500     /* countdown granularity on the veil           */
#define FLUSH_MS    200     /* air time for the final DRIVE 0,0 frame      */

typedef enum {
    PWR_QUIET = 0,          /* idle clock below the warn threshold         */
    PWR_WARN,               /* veil up, counting down                      */
} pwr_state_t;

static pwr_state_t s_state;
static lv_obj_t *s_veil;            /* full-screen dim veil on layer_top   */
static lv_obj_t *s_count_lbl;       /* the big seconds number              */
static lv_timer_t *s_tick;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t timeout_ms(void)
{
    return (int64_t)CONFIG_SCR_AUTO_OFF_IDLE_MIN * 60 * 1000;
}

static int64_t warn_ms(void)
{
    return (int64_t)CONFIG_SCR_AUTO_OFF_WARN_S * 1000;
}

/* ---- the countdown veil (created lazily, lives on lv_layer_top) ------------*/
static void veil_ensure(void)
{
    if (s_veil != NULL) {
        return;
    }
    s_veil = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_veil, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_veil, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_veil, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_veil, 0, 0);
    lv_obj_set_style_radius(s_veil, 0, 0);
    lv_obj_remove_flag(s_veil, LV_OBJ_FLAG_SCROLLABLE);
    /* clickable: swallow the cancel tap so it never reaches the page below */
    lv_obj_add_flag(s_veil, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ic = lv_label_create(s_veil);
    lv_label_set_text(ic, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(ic, F_XXL, 0);
    ui_label_set_color(ic, lv_color_hex(UI_COL_ACCENT));
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 90);

    lv_obj_t *title = lv_label_create(s_veil);
    lv_label_set_text(title, "AUTO POWER OFF");
    lv_obj_set_style_text_font(title, F_LG, 0);
    ui_label_set_color(title, lv_color_hex(UI_COL_TXT));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 168);

    s_count_lbl = lv_label_create(s_veil);
    lv_label_set_text(s_count_lbl, "--");
    lv_obj_set_style_text_font(s_count_lbl, F_XXL, 0);
    ui_label_set_color(s_count_lbl, lv_color_hex(UI_COL_WARN));
    lv_obj_align(s_count_lbl, LV_ALIGN_TOP_MID, 0, 226);

    lv_obj_t *hint = lv_label_create(s_veil);
    lv_label_set_text(hint, "TOUCH TO CANCEL");
    lv_obj_set_style_text_font(hint, F_MD, 0);
    ui_label_set_color(hint, lv_color_hex(UI_COL_INFO));
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 310);

    lv_obj_t *sub = lv_label_create(s_veil);
    lv_label_set_text(sub, "then press BOOT to power on");
    lv_obj_set_style_text_font(sub, F_SM, 0);
    ui_label_set_color(sub, lv_color_hex(UI_COL_DIM));
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 348);
}

static void veil_show(int64_t left_ms)
{
    veil_ensure();
    int left_s = (int)((left_ms + 999) / 1000);
    if (left_s < 1) {
        left_s = 1;
    }
    ui_label_set_fmt(s_count_lbl, "%d", left_s);
    if (lv_obj_has_flag(s_veil, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_veil);
    }
}

static void veil_hide(void)
{
    if (s_veil != NULL && !lv_obj_has_flag(s_veil, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- power off ----------------------------------------------------------------*/
static void power_off(int64_t idle_ms)
{
    ESP_LOGI(TAG, "auto power-off after %lld s idle",
             (long long)(idle_ms / 1000));
    app_state_log(SCR_LOG_INFO, "Auto power-off (%lld min idle)",
                  (long long)(idle_ms / 60000));

    /* Best-effort vehicle stop before the radio dies.  The C6 fails safe on
     * radio loss either way, but a deliberate STOP latch + 0,0 frame beats a
     * watchdog timeout.  Safe from the LVGL task (the on-screen STOP button
     * calls it from the same context). */
    scr_ctrl_stop_button();

    veil_hide();
    vTaskDelay(pdMS_TO_TICKS(FLUSH_MS));

    bsp_display_backlight_off();
    /* BOOT (GPIO0) pressed = low: ext0 wake.  Deep-sleep wake reboots into
     * the normal boot path; app_main logs the wake reason as "Power on". */
    esp_sleep_enable_ext0_wakeup(BSP_BUTTON_BOOT_IO, 0);
    esp_deep_sleep_start();                 /* never returns */
}

/* ---- 500 ms watchdog -----------------------------------------------------------*/
static void power_tick(lv_timer_t *t)
{
    (void)t;
    svc_ota_t ota;
    scr_svc_get_ota(&ota);
    bool ota_busy = ota.phase == SVC_OTA_VERIFY ||
                    ota.phase == SVC_OTA_SEND ||
                    ota.phase == SVC_OTA_WAIT_TC;
    if (ota_busy) {
        s_state = PWR_QUIET;
        veil_hide();
        return;
    }

    int64_t idle = (int64_t)lv_display_get_inactive_time(NULL);
    int64_t warn_at = timeout_ms() - warn_ms();

    if (idle < warn_at) {
        if (s_state != PWR_QUIET) {
            s_state = PWR_QUIET;
            veil_hide();
        }
        return;
    }

    int64_t left = timeout_ms() - idle;
    if (left <= 0) {
        power_off(idle);
        return;
    }
    s_state = PWR_WARN;
    veil_show(left);
}

/* ---- lifecycle -----------------------------------------------------------------*/
void scr_power_start(void)
{
    s_state = PWR_QUIET;
    s_veil = NULL;
    s_tick = lv_timer_create(power_tick, TICK_MS, NULL);
    ESP_LOGI(TAG, "auto power-off armed: %d min idle, %d s warning",
             (int)CONFIG_SCR_AUTO_OFF_IDLE_MIN, (int)CONFIG_SCR_AUTO_OFF_WARN_S);
}

#else /* !CONFIG_SCR_AUTO_OFF_ENABLE */

void scr_power_start(void) { }

#endif /* CONFIG_SCR_AUTO_OFF_ENABLE */
