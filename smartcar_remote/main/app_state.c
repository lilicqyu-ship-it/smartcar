/*
 * app_state.c - Single Source of Truth implementation (spec 98-101)
 *
 * One mutex-protected struct; every consumer works on a snapshot copy, so no
 * page ever sees a torn mix of link/telemetry/control data.  Staleness is
 * evaluated at snapshot time, never at write time.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "app_state.h"

static const char *TAG = "scr_state";

static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_log_mtx;
static scr_state_t s_state;

/* telemetry EMA bookkeeping (mirrors esp32c6_car assets app.js speedometer) */
static int64_t s_last_tele_ms;
static bool    s_tele_seeded;

/* video/vision freshness stamps (evaluated at snapshot time, never at write) */
static int64_t s_last_cam_frame_ms;
static int64_t s_last_vision_ms;

/* alert slots: see the alerts section at the bottom of this file.
 * 8 slots since the camera plane (design doc 6): camera/link alerts may
 * co-exist with radio/battery/fault alerts without clobbering. */
#define SCR_ALERT_SLOTS 8

typedef struct {
    uint32_t          id;
    bool              used;
    bool              ack;
    scr_alert_level_t level;
    char              title[SCR_ALERT_TITLE_MAX];
    char              msg[SCR_ALERT_MSG_MAX];
} alert_slot_t;

static alert_slot_t s_alerts[SCR_ALERT_SLOTS];

/* event ring (spec 83) */
static scr_event_t s_ring[SCR_EVENT_RING_LEN];
static int s_ring_head;                 /* next write slot */
static int s_ring_count;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void app_state_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_log_mtx = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
    memset(s_alerts, 0, sizeof(s_alerts));
    memset(s_ring, 0, sizeof(s_ring));

    s_state.conn     = SCR_CONN_NONE;
    s_state.owner    = SCR_OWNER_NONE;
    s_state.quality  = SCR_QUAL_UNKNOWN;
    s_state.mode     = SCR_MODE_NORMAL;
    s_state.rssi     = 0;
    s_state.channel  = 0;
    s_state.c6_fw[0] = '-';
    s_state.cam.sensor[0] = '-';
    s_last_cam_frame_ms = 0;
    s_last_vision_ms    = 0;
}

void app_state_snapshot(scr_state_t *out)
{
    int64_t now = now_ms();

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_state;
    out->uptime_ms   = (uint32_t)now;
    out->tele_fresh  = (s_last_tele_ms != 0) &&
                       ((now - s_last_tele_ms) < CONFIG_SCR_TELE_TIMEOUT_MS);
    /* video: subscribed but no decodable frame inside the window -> STALE.
     * s_last_cam_frame_ms == 0 means "never received a frame since the
     * subscription started", which is stale by the same rule. */
    out->cam.stale   = out->cam.subscribed &&
                       ((now - s_last_cam_frame_ms) > CONFIG_SCR_CAM_FRAME_TIMEOUT_MS);
    out->vision.fresh = (s_last_vision_ms != 0) &&
                        ((now - s_last_vision_ms) < CONFIG_SCR_VISION_STALE_MS);
    xSemaphoreGive(s_mtx);
}

/* ---- system state derivation (spec 59) ------------------------------------*/
scr_sys_t app_state_derive(const scr_state_t *s)
{
    if (!s->boot_sys) {
        return SCR_SYS_BOOT;
    }
    if (s->emerg_latch) {
        return SCR_SYS_EMERGENCY;
    }
    if (s->fault_code != 0) {
        return SCR_SYS_FAULT;
    }
    if (s->alert.level == SCR_ALERT_WARNING || s->alert.level == SCR_ALERT_CRITICAL) {
        return SCR_SYS_WARNING;
    }
    if (s->conn != SCR_CONN_CONNECTED || !s->tc_on) {
        return SCR_SYS_CONNECTING;
    }
    if (s->stop_latch) {
        return SCR_SYS_STOPPED;
    }
    if (s->out_v != 0 || s->out_w != 0) {
        return SCR_SYS_CONTROL;
    }
    return SCR_SYS_READY;
}

/* ---- link setters -----------------------------------------------------------*/
void app_state_set_conn(scr_conn_t c)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.conn = c;
    xSemaphoreGive(s_mtx);
}

void app_state_set_ctrl_role(bool ctrl)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.ctrl_role = ctrl;
    if (ctrl) {
        s_state.owner = SCR_OWNER_S3;
    } else if (s_state.owner == SCR_OWNER_S3) {
        s_state.owner = SCR_OWNER_NONE;
    }
    xSemaphoreGive(s_mtx);
}

void app_state_set_tc(bool on)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.tc_on = on;
    xSemaphoreGive(s_mtx);
}

void app_state_set_wifi(int8_t rssi, uint8_t ch)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.rssi    = rssi;
    s_state.channel = ch;
    xSemaphoreGive(s_mtx);
}

void app_state_set_latency(uint16_t ms)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.lat_ms = ms;
    if (ms != 0 && (s_state.lat_min == 0 || ms < s_state.lat_min)) {
        s_state.lat_min = ms;
    }
    if (ms > s_state.lat_max) {
        s_state.lat_max = ms;
    }
    xSemaphoreGive(s_mtx);
}

void app_state_set_rates(uint16_t tx, uint16_t rx)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.tx_rate = tx;
    s_state.rx_rate = rx;
    xSemaphoreGive(s_mtx);
}

void app_state_set_loss(uint16_t pct_x10)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.loss_pct_x10 = pct_x10;
    xSemaphoreGive(s_mtx);
}

void app_state_set_quality(scr_qual_t q)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.quality = q;
    xSemaphoreGive(s_mtx);
}

void app_state_set_c6_fw(const char *fw)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    /* C6 reports bare SemVer ("0.1.3"); show it as "v0.1.3" like the S3 */
    bool bare = fw && fw[0] >= '0' && fw[0] <= '9';
    snprintf(s_state.c6_fw, sizeof(s_state.c6_fw), "%s%s", bare ? "v" : "", fw ? fw : "-");
    s_state.c6_fw_seq++;
    xSemaphoreGive(s_mtx);
}

void app_state_set_tc_ver(const char *app, const char *sbl)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    snprintf(s_state.tc_app_ver, sizeof(s_state.tc_app_ver), "%s", app ? app : "");
    snprintf(s_state.tc_sbl_ver, sizeof(s_state.tc_sbl_ver), "%s", sbl ? sbl : "");
    s_state.tc_ver_seq++;
    xSemaphoreGive(s_mtx);
}

void app_state_set_owner(scr_owner_t o)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.owner = o;
    xSemaphoreGive(s_mtx);
}

void app_state_set_pair_status(const char *txt)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    snprintf(s_state.pair_status, sizeof(s_state.pair_status), "%s", txt ? txt : "");
    xSemaphoreGive(s_mtx);
}

/* ---- video / vision setters ---------------------------------------------------
 * Counters and stats live in scr_cam (single owner); these setters only mirror
 * them into the snapshot.  Freshness is deliberately NOT set here - the
 * snapshot recomputes stale/fresh from the timestamps below (same pattern as
 * tele_fresh, spec 101). */
void app_state_set_cam_conn(scr_cam_conn_t c)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.cam.conn = c;
    xSemaphoreGive(s_mtx);
}

void app_state_set_cam_subscribed(bool on)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.cam.subscribed = on;
    if (on) {
        s_last_cam_frame_ms = 0;    /* new subscription: demand a fresh frame */
    }
    xSemaphoreGive(s_mtx);
}

void app_state_set_cam_hello(const char *sensor, uint16_t w, uint16_t h)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    snprintf(s_state.cam.sensor, sizeof(s_state.cam.sensor), "%s", sensor ? sensor : "?");
    s_state.cam.w = w;
    s_state.cam.h = h;
    xSemaphoreGive(s_mtx);
}

void app_state_set_cam_stats(uint8_t fps_x10, uint16_t decode_ms_max, uint16_t e2e_ms)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.cam.fps_x10 = fps_x10;
    s_state.cam.decode_ms_max = decode_ms_max;
    s_state.cam.e2e_ms = e2e_ms;
    xSemaphoreGive(s_mtx);
}

void app_state_set_cam_rtt(uint16_t rtt_ms)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.cam.ping_rtt_ms = rtt_ms;
    xSemaphoreGive(s_mtx);
}

void app_state_set_cam_counters(uint32_t seq, uint32_t drop,
                                uint32_t frame_err, uint32_t decode_err)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.cam.seq = seq;
    s_state.cam.drop = drop;
    s_state.cam.frame_err = frame_err;
    s_state.cam.decode_err = decode_err;
    xSemaphoreGive(s_mtx);
}

void app_state_note_cam_frame(void)
{
    int64_t now = now_ms();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_last_cam_frame_ms = now;
    xSemaphoreGive(s_mtx);
}

void app_state_set_vision_mode(uint8_t mode)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.vision.mode = mode;
    if (mode == 0 /* VISION_MODE_IDX_OFF */) {
        s_state.vision.valid = false;
        s_state.vision.objects_count = 0;
        s_last_vision_ms = 0;
    }
    xSemaphoreGive(s_mtx);
}

void app_state_set_vision_result(const scr_vision_result_t *v)
{
    if (v == NULL) {
        return;
    }
    int64_t now = now_ms();

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.vision.mode          = v->mode;
    s_state.vision.valid         = v->valid;
    s_state.vision.confidence    = v->confidence;
    s_state.vision.cx            = v->cx;
    s_state.vision.error_x1000   = v->error_x1000;
    s_state.vision.angle_x10     = v->angle_x10;
    s_state.vision.ts_ms         = v->ts_ms;
    s_state.vision.objects_count = v->objects_count;
    s_last_vision_ms = now;
    xSemaphoreGive(s_mtx);
}

void app_state_set_drive_mode(uint8_t mode)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.vision.drive_mode = mode;
    xSemaphoreGive(s_mtx);
}

void app_state_set_telemetry(const proto_telemetry_t *t)
{
    if (t == NULL) {
        return;
    }
    int64_t now = now_ms();

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    s_state.tele_seq   = t->seq;
    s_state.v_target_l = t->v_target_l;
    s_state.v_target_r = t->v_target_r;
    s_state.v_meas_l   = t->v_meas_l;
    s_state.v_meas_r   = t->v_meas_r;
    s_state.batt_pct   = t->battery_pct;
    s_state.batt_mv    = t->battery_mv;
    s_state.odo_session_mm = t->odo_session_mm;
    s_state.odo_total_mm   = t->odo_total_mm;
    s_state.fault_code = t->fault_code;
    s_state.veh_state  = t->state;
    s_state.tc_fw_ver  = t->fw_ver;
    s_state.hw_rev     = t->hw_rev;
    s_state.link_rtt_ms   = t->link_rtt_ms;
    s_state.link_err_rate = t->link_err_rate;
    /* telemetry only exists while the C6<->TC275 SPI link is up, so a frame
     * is first-hand proof of it - self-heals a missed {"t":"tc"} edge */
    s_state.tc_on = true;

    /* body speed EMA: mean measured wheel speed, dt-aware, snap on gaps and
     * around rest (same tuning as the phone control page) */
    int32_t v = ((int32_t)t->v_meas_l + (int32_t)t->v_meas_r) / 2;
    if (s_last_tele_ms != 0) {
        int64_t dt = now - s_last_tele_ms;
        if (dt > 0 && dt <= 1000) {
            if (!s_tele_seeded || dt > 400 || (v > -30 && v < 30)) {
                s_state.speed_mm_s = v;
                s_tele_seeded = true;
            } else {
                int32_t cur = s_state.speed_mm_s;
                cur += (int32_t)(((float)(v - cur)) * (1.0f - expf(-(float)dt / 150.0f)));
                s_state.speed_mm_s = cur;
            }
        } else {
            s_state.speed_mm_s = v;   /* first frame or long gap: snap */
            s_tele_seeded = true;
        }
    } else {
        s_state.speed_mm_s = v;
        s_tele_seeded = true;
    }
    s_last_tele_ms = now;

    xSemaphoreGive(s_mtx);
}

/* ---- control setters ---------------------------------------------------------*/
void app_state_set_joy(int16_t v, int16_t w)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.joy_v = v;
    s_state.joy_w = w;
    xSemaphoreGive(s_mtx);
}

void app_state_set_out(int16_t v, int16_t w)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.out_v = v;
    s_state.out_w = w;
    xSemaphoreGive(s_mtx);
}

void app_state_set_stop(bool on)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.stop_latch = on;
    xSemaphoreGive(s_mtx);
}

void app_state_set_emerg(bool on)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.emerg_latch = on;
    xSemaphoreGive(s_mtx);
}

void app_state_set_mode(scr_mode_t m)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.mode = m;
    xSemaphoreGive(s_mtx);
}

/* ---- boot marks -----------------------------------------------------------------*/
static void boot_mark(bool *field)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *field = true;
    xSemaphoreGive(s_mtx);
}

void app_state_boot_mark_lcd(void)   { boot_mark(&s_state.boot_lcd); }
void app_state_boot_mark_touch(void) { boot_mark(&s_state.boot_touch); }
void app_state_boot_mark_radio(void) { boot_mark(&s_state.boot_radio); }
void app_state_boot_mark_sys(void)   { boot_mark(&s_state.boot_sys); }

/* ---- event ring ------------------------------------------------------------------*/
void app_state_log(scr_log_level_t lvl, const char *fmt, ...)
{
    va_list ap;
    char text[SCR_EVENT_TEXT_MAX];

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    xSemaphoreTake(s_log_mtx, portMAX_DELAY);
    scr_event_t *e = &s_ring[s_ring_head];
    e->ts_ms = (uint32_t)(now_ms());
    e->level = lvl;
    snprintf(e->text, sizeof(e->text), "%s", text);
    s_ring_head = (s_ring_head + 1) % SCR_EVENT_RING_LEN;
    if (s_ring_count < SCR_EVENT_RING_LEN) {
        s_ring_count++;
    }
    xSemaphoreGive(s_log_mtx);

    /* mirror to the serial console at the matching level */
    switch (lvl) {
        case SCR_LOG_CRIT:   ESP_LOGE(TAG, "%s", text); break;
        case SCR_LOG_WARN:   ESP_LOGW(TAG, "%s", text); break;
        case SCR_LOG_NOTICE: ESP_LOGI(TAG, "%s", text); break;
        default:             ESP_LOGI(TAG, "%s", text); break;
    }
}

int app_state_events_get(scr_event_t *out, int cap)
{
    int n = 0;

    if (out == NULL || cap <= 0) {
        return 0;
    }
    xSemaphoreTake(s_log_mtx, portMAX_DELAY);
    for (int i = 0; i < s_ring_count && n < cap; i++) {
        int idx = (s_ring_head - 1 - i + SCR_EVENT_RING_LEN * 2) % SCR_EVENT_RING_LEN;
        out[n++] = s_ring[idx];
    }
    xSemaphoreGive(s_log_mtx);
    return n;
}

/* ---- alerts ------------------------------------------------------------------------
 * Per-id slots: simultaneous alerts never clobber each other, and clear(id)
 * always removes exactly its own condition.  The snapshot surfaces the slot
 * with the highest severity (newest wins a tie).  The table itself lives with
 * the other statics at the top of this file. */

static alert_slot_t *alert_slot(uint32_t id)
{
    for (int i = 0; i < SCR_ALERT_SLOTS; i++) {
        if (s_alerts[i].used && s_alerts[i].id == id) {
            return &s_alerts[i];
        }
    }
    return NULL;
}

static void alerts_compose_locked(scr_alert_t *out, bool *ack)
{
    /* highest severity wins; enum: INFO=1 < NOTICE < WARNING < CRITICAL */
    alert_slot_t *best = NULL;
    for (int i = 0; i < SCR_ALERT_SLOTS; i++) {
        alert_slot_t *s = &s_alerts[i];
        if (!s->used) {
            continue;
        }
        if (best == NULL || s->level >= best->level) {
            best = s;
        }
    }
    if (best == NULL) {
        memset(out, 0, sizeof(*out));
        *ack = false;
        return;
    }
    out->id    = best->id;
    out->level = best->level;
    snprintf(out->title, sizeof(out->title), "%s", best->title);
    snprintf(out->msg, sizeof(out->msg), "%s", best->msg);
    *ack = best->ack;
}

void app_alert_raise(uint32_t id, scr_alert_level_t lvl,
                     const char *title, const char *fmt, ...)
{
    va_list ap;
    char msg[SCR_ALERT_MSG_MAX];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    alert_slot_t *s = alert_slot(id);
    if (s == NULL) {
        for (int i = 0; i < SCR_ALERT_SLOTS; i++) {
            if (!s_alerts[i].used) {
                s = &s_alerts[i];
                break;
            }
        }
        if (s == NULL) {
            xSemaphoreGive(s_mtx);
            return;                     /* table full: keep existing alerts */
        }
        s->id   = id;
        s->used = true;
        s->ack  = false;                /* fresh condition: demand ack again */
    }
    s->level = lvl;
    snprintf(s->title, sizeof(s->title), "%s", title);
    snprintf(s->msg, sizeof(s->msg), "%s", msg);
    alerts_compose_locked(&s_state.alert, &s_state.alert_ack);
    xSemaphoreGive(s_mtx);
}

void app_alert_clear(uint32_t id)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    alert_slot_t *s = alert_slot(id);
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
    alerts_compose_locked(&s_state.alert, &s_state.alert_ack);
    xSemaphoreGive(s_mtx);
}

void app_alert_ack(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    /* acknowledge exactly what the overlay is showing (the composed alert) */
    alert_slot_t *s = alert_slot(s_state.alert.id);
    if (s != NULL) {
        s->ack = true;
    }
    s_state.alert_ack = true;
    xSemaphoreGive(s_mtx);
}
