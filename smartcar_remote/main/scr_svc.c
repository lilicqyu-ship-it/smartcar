/*
 * scr_svc.c - core-0 service task: firmware staging + OTA upload, C6
 * /api/diag polling, TC275 calibration commands / events.
 * Design: doc/08-architecture-v2.md.  Interface facts: esp32c6_car doc 07/08/09/17.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "sdkconfig.h"

/* bool Kconfig symbols are undefined when n; fold into a 0/1 constant */
#ifdef CONFIG_SCR_OTA_NO_AUTH
#define CONFIG_SCR_OTA_NO_AUTH_ON 1
#else
#define CONFIG_SCR_OTA_NO_AUTH_ON 0
#endif
#include "esp_http_client.h"
#include "esp_rom_crc.h"
#include "cJSON.h"
#include "sdkconfig.h"

#include "scr_svc.h"
#include "scr_link.h"
#include "scr_settings.h"
#include "app_state.h"
#include "proto/proto_frames.h"

static const char *TAG = "scr_svc";

#define SVC_CORE            0
#define SVC_PRIO            3
#define SVC_STACK           6144
#define DIAG_PERIOD_MS      2000
#define DIAG_TIMEOUT_MS     1500
#define OTA_CHUNK           4096
#define OTA_MAX             (3u * 1024u * 1024u)
#define STAGE_HDR_SIZE      4096u
#define TC_WAIT_MS          90000
#define OTA_STILL_MM_S      50
/* C6 accepted the image -> reboot delay + boot + softAP up + the S3 side
 * (10 s RX watchdog, WS retries, maybe a Wi-Fi re-association).  Within this
 * window a dead S3<->C6 link is the expected OTA reboot, not RADIO LOST. */
#define C6_RECONNECT_GRACE_MS 90000

#define SCFW_MAGIC          "SCFW"

typedef struct __attribute__((packed)) {
    char     magic[4];
    uint16_t hdr_ver;
    uint8_t  target;        /* 1 = C6, 2 = TC275 */
    uint8_t  rsv;
    uint32_t size;
    uint32_t crc32;
    char     version[32];
    char     built[32];
} scfw_hdr_t;

typedef enum { OP_RESCAN = 1, OP_OTA, OP_DIAG } svc_op_t;

static struct {
    SemaphoreHandle_t mtx;
    QueueHandle_t     q;
    const esp_partition_t *part[SVC_FW_COUNT];
    svc_stage_t  stage[SVC_FW_COUNT];
    svc_ota_t    ota;
    int64_t      tc_wait_since;
    svc_c6diag_t diag;
    int64_t      diag_ok_ms;
    volatile bool diag_enabled;
    svc_cal_t    cal;
    int64_t      ota_done_ms;   /* C6 OTA reached DONE (reboot grace anchor) */
    uint8_t      io[OTA_CHUNK];     /* only touched by the svc task */
} s;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

#define LOCK()   xSemaphoreTake(s.mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s.mtx)

/* ---- staging --------------------------------------------------------------------*/
static void stage_scan_one(svc_fw_t t)
{
    svc_stage_t st = { 0 };
    const esp_partition_t *p = s.part[t];
    scfw_hdr_t h;

    if (p == NULL) {
        st.state = SVC_STAGE_BAD;
        strlcpy(st.why, "partition missing - reflash table", sizeof(st.why));
        goto out;
    }
    if (esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) {
        st.state = SVC_STAGE_BAD;
        strlcpy(st.why, "flash read error", sizeof(st.why));
        goto out;
    }
    if (memcmp(h.magic, SCFW_MAGIC, 4) != 0) {
        st.state = SVC_STAGE_EMPTY;
        goto out;
    }
    st.size = h.size;
    st.crc32 = h.crc32;
    memcpy(st.version, h.version, sizeof(st.version));
    st.version[sizeof(st.version) - 1] = '\0';
    memcpy(st.built, h.built, sizeof(st.built));
    st.built[sizeof(st.built) - 1] = '\0';
    if (h.hdr_ver != 1 || h.target != (uint8_t)(t + 1)) {
        st.state = SVC_STAGE_BAD;
        strlcpy(st.why, "header target / version mismatch", sizeof(st.why));
        goto out;
    }
    if (h.size == 0 || h.size > OTA_MAX || h.size > p->size - STAGE_HDR_SIZE) {
        st.state = SVC_STAGE_BAD;
        strlcpy(st.why, "size out of range", sizeof(st.why));
        goto out;
    }
    /* full CRC: 3 MB read at low prio on core 0 - never on the UI core */
    uint32_t crc = 0;
    for (uint32_t off = 0; off < h.size; off += OTA_CHUNK) {
        uint32_t n = h.size - off < OTA_CHUNK ? h.size - off : OTA_CHUNK;
        if (esp_partition_read(p, STAGE_HDR_SIZE + off, s.io, n) != ESP_OK) {
            st.state = SVC_STAGE_BAD;
            strlcpy(st.why, "flash read error", sizeof(st.why));
            goto out;
        }
        crc = esp_rom_crc32_le(crc, s.io, n);
    }
    if (crc != h.crc32) {
        st.state = SVC_STAGE_BAD;
        snprintf(st.why, sizeof(st.why), "CRC mismatch %08lx", (unsigned long)crc);
        goto out;
    }
    st.state = SVC_STAGE_OK;
out:
    LOCK();
    s.stage[t] = st;
    UNLOCK();
}

void scr_svc_get_stage(svc_fw_t t, svc_stage_t *out)
{
    LOCK();
    *out = s.stage[t];
    UNLOCK();
}

void scr_svc_rescan_stage(void)
{
    uint8_t op = OP_RESCAN;
    (void)xQueueSend(s.q, &op, 0);
}

/* ---- OTA --------------------------------------------------------------------------*/
static void ota_set(svc_ota_phase_t ph, uint8_t pct, const char *msg)
{
    LOCK();
    s.ota.phase = ph;
    s.ota.pct = pct;
    if (ph == SVC_OTA_DONE && s.ota.target == SVC_FW_C6) {
        s.ota_done_ms = now_ms();
    }
    if (msg) {
        strlcpy(s.ota.msg, msg, sizeof(s.ota.msg));
    }
    UNLOCK();
}

void scr_svc_get_ota(svc_ota_t *out)
{
    LOCK();
    /* TC275 relay: give up waiting for otastatus eventually */
    if (s.ota.phase == SVC_OTA_WAIT_TC && now_ms() - s.tc_wait_since > TC_WAIT_MS) {
        s.ota.phase = SVC_OTA_FAILED;
        strlcpy(s.ota.msg, "TC275 did not report completion (check its serial log)",
                sizeof(s.ota.msg));
    }
    *out = s.ota;
    UNLOCK();
}

bool scr_svc_ota_quiet_c6(void)
{
    bool quiet = false;
    LOCK();
    if (s.ota.target == SVC_FW_C6) {
        if (s.ota.phase == SVC_OTA_VERIFY || s.ota.phase == SVC_OTA_SEND) {
            quiet = true;      /* bulk upload may starve telemetry for seconds */
        } else if (s.ota.phase == SVC_OTA_DONE && s.ota_done_ms != 0 &&
                   now_ms() - s.ota_done_ms < C6_RECONNECT_GRACE_MS) {
            quiet = true;      /* C6 is rebooting into the new image */
        }
    }
    UNLOCK();
    return quiet;
}

bool scr_svc_ota_start(svc_fw_t t, char *why, int why_cap)
{
    scr_state_t st;
    app_state_snapshot(&st);
    scr_settings_t set;
    scr_settings_get(&set);
    svc_stage_t sg;
    scr_svc_get_stage(t, &sg);
    svc_ota_t o;
    scr_svc_get_ota(&o);

    const char *err = NULL;
    if (o.phase == SVC_OTA_VERIFY || o.phase == SVC_OTA_SEND || o.phase == SVC_OTA_WAIT_TC) {
        err = "an update is already running";
    } else if (sg.state != SVC_STAGE_OK) {
        err = "no verified image in the staging area";
    } else if (st.conn != SCR_CONN_CONNECTED || !st.ctrl_role) {
        err = "need a live link holding CTRL";
    } else if (!CONFIG_SCR_OTA_NO_AUTH_ON && set.token[0] == '\0') {
        err = "not paired: the C6 rejects OTA without a token";
    } else if (st.tele_fresh && abs(st.speed_mm_s) > OTA_STILL_MM_S) {
        err = "vehicle is moving - stop it first";
    } else if (t == SVC_FW_TC && !st.tc_on) {
        err = "TC275 link is down";
    }
    if (err) {
        if (why) {
            strlcpy(why, err, why_cap);
        }
        return false;
    }
    LOCK();
    memset(&s.ota, 0, sizeof(s.ota));
    s.ota_done_ms = 0;
    s.ota.target = t;
    s.ota.phase = SVC_OTA_VERIFY;
    strlcpy(s.ota.msg, "verifying staged image", sizeof(s.ota.msg));
    UNLOCK();
    uint8_t op = OP_OTA;
    (void)xQueueSend(s.q, &op, 0);
    return true;
}

static void ota_run(void)
{
    svc_fw_t t;
    LOCK();
    t = s.ota.target;
    UNLOCK();

    /* re-verify the image right before sending it */
    stage_scan_one(t);
    svc_stage_t sg;
    scr_svc_get_stage(t, &sg);
    if (sg.state != SVC_STAGE_OK) {
        ota_set(SVC_OTA_FAILED, 0, sg.state == SVC_STAGE_EMPTY ? "staging area empty" : sg.why);
        return;
    }

    /* hold the car: DRIVE keeps flowing as a 0/0 heartbeat */
    app_state_set_stop(true);
    app_state_log(SCR_LOG_WARN, "OTA %s start (%lu B, %s)", t == SVC_FW_C6 ? "C6" : "TC275",
                  (unsigned long)sg.size, sg.version);

    scr_settings_t set;
    scr_settings_get(&set);
    char url[64];
    snprintf(url, sizeof(url), "http://%s/ota/%s", CONFIG_SCR_C6_IP,
             t == SVC_FW_C6 ? "c6" : "tc275");
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 35000,        /* relay finish waits up to 30 s for ACKs */
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        ota_set(SVC_OTA_FAILED, 0, "http client init failed (memory)");
        return;
    }
    if (set.token[0] != '\0') {
        esp_http_client_set_header(c, "X-Session-Token", set.token);
    }
    esp_http_client_set_header(c, "Content-Type", "application/octet-stream");

    ota_set(SVC_OTA_SEND, 0, "uploading");
    esp_err_t err = esp_http_client_open(c, (int)sg.size);
    if (err != ESP_OK) {
        ota_set(SVC_OTA_FAILED, 0, "cannot connect to the C6");
        esp_http_client_cleanup(c);
        return;
    }
    const esp_partition_t *p = s.part[t];
    uint32_t sent = 0;
    bool ok = true;
    while (sent < sg.size) {
        uint32_t n = sg.size - sent < OTA_CHUNK ? sg.size - sent : OTA_CHUNK;
        if (esp_partition_read(p, STAGE_HDR_SIZE + sent, s.io, n) != ESP_OK) {
            ota_set(SVC_OTA_FAILED, 0, "flash read error during upload");
            ok = false;
            break;
        }
        int w = esp_http_client_write(c, (const char *)s.io, (int)n);
        if (w != (int)n) {
            ota_set(SVC_OTA_FAILED, 0, "upload interrupted (link dropped?)");
            ok = false;
            break;
        }
        sent += n;
        LOCK();
        s.ota.pct = (uint8_t)((uint64_t)sent * 100u / sg.size);
        UNLOCK();
    }

    int status = 0;
    char resp[192] = { 0 };
    if (ok) {
        (void)esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        int r = esp_http_client_read_response(c, resp, sizeof(resp) - 1);
        if (r > 0) {
            resp[r] = '\0';
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) {
        app_state_log(SCR_LOG_WARN, "OTA upload failed");
        return;
    }

    LOCK();
    s.ota.http_status = status;
    UNLOCK();

    cJSON *root = cJSON_Parse(resp);
    const cJSON *e = root ? cJSON_GetObjectItem(root, "e") : NULL;
    const char *etxt = cJSON_IsString(e) ? e->valuestring : "";
    char msg[96];
    if (status == 200) {
        if (t == SVC_FW_C6) {
            const cJSON *rb = root ? cJSON_GetObjectItem(root, "reboot_s") : NULL;
            snprintf(msg, sizeof(msg), "C6 accepted - rebooting in %d s, link will reconnect",
                     cJSON_IsNumber(rb) ? rb->valueint : 5);
            ota_set(SVC_OTA_DONE, 100, msg);
        } else {
            LOCK();
            s.ota.phase = SVC_OTA_WAIT_TC;
            s.ota.pct = 100;
            s.tc_wait_since = now_ms();
            strlcpy(s.ota.msg, "uploaded - TC275 is writing its slot", sizeof(s.ota.msg));
            UNLOCK();
        }
        app_state_log(SCR_LOG_INFO, "OTA upload accepted (HTTP 200)");
    } else {
        const char *hint =
            status == 401 ? "not paired / token rejected - pair again" :
            status == 413 ? "image too large for the C6" :
            status == 503 ? "C6 not ready (TC275 link down or OTA busy)" :
            status == 507 ? "C6 out of memory" :
            status == 502 ? "C6 rejected the image (signature / write)" :
            "unexpected reply";
        snprintf(msg, sizeof(msg), "HTTP %d %s: %s", status, etxt, hint);
        ota_set(SVC_OTA_FAILED, 0, msg);
        app_state_log(SCR_LOG_WARN, "OTA failed HTTP %d %s", status, etxt);
    }
    cJSON_Delete(root);
}

/* ---- C6 /api/diag -------------------------------------------------------------------*/
static uint32_t j_u32(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
}

static void j_str(const cJSON *o, const char *k, char *dst, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    strlcpy(dst, cJSON_IsString(v) ? v->valuestring : "-", cap);
}

static void diag_poll(void)
{
    char url[48];
    snprintf(url, sizeof(url), "http://%s/api/diag", CONFIG_SCR_C6_IP);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = DIAG_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return;
    }
    char *buf = (char *)s.io;
    int len = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        (void)esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            len = esp_http_client_read_response(c, buf, OTA_CHUNK - 1);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    cJSON *root = (len > 0) ? (buf[len] = '\0', cJSON_Parse(buf)) : NULL;
    if (root == NULL) {
        LOCK();
        s.diag.valid = false;
        UNLOCK();
        return;
    }
    svc_c6diag_t d;
    LOCK();
    d = s.diag;
    UNLOCK();
    uint32_t prev_crc = d.crc_err, prev_fmt = d.fmt_err;
    bool had = d.valid || d.age_ms >= 0;

    j_str(root, "ver", d.ver, sizeof(d.ver));
    j_str(root, "state", d.state, sizeof(d.state));
    j_str(root, "slot", d.slot, sizeof(d.slot));
    j_str(root, "pair", d.pair, sizeof(d.pair));
    d.reset = j_u32(root, "reset");
    d.selfcheck = j_u32(root, "selfcheck");
    d.coredump = cJSON_IsTrue(cJSON_GetObjectItem(root, "coredump"));
    d.factory = cJSON_IsTrue(cJSON_GetObjectItem(root, "factory"));
    d.heap_min = j_u32(root, "heap_min");
    d.uptime_s = j_u32(root, "uptime_s");
    d.cli = (int)j_u32(root, "cli");
    d.zs = j_u32(root, "zs");
    const cJSON *l = cJSON_GetObjectItem(root, "link");
    if (cJSON_IsObject(l)) {
        d.link_up = cJSON_IsTrue(cJSON_GetObjectItem(l, "up"));
        d.link_rtt = j_u32(l, "rtt");
        d.crc_err = j_u32(l, "crc_err");
        d.fmt_err = j_u32(l, "fmt_err");
        d.rx = j_u32(l, "rx");
        d.tx = j_u32(l, "tx");
        d.busy = j_u32(l, "busy");
    }
    d.crc_err_delta = (had && d.crc_err >= prev_crc) ? d.crc_err - prev_crc : 0;
    d.fmt_err_delta = (had && d.fmt_err >= prev_fmt) ? d.fmt_err - prev_fmt : 0;
    const cJSON *imu = cJSON_GetObjectItem(root, "imu");
    d.imu = cJSON_IsObject(imu) ? (cJSON_IsTrue(cJSON_GetObjectItem(imu, "ok")) ? 1 : 0) : -1;
    d.valid = true;
    cJSON_Delete(root);

    LOCK();
    s.diag = d;
    s.diag_ok_ms = now_ms();
    UNLOCK();
}

void scr_svc_diag_poll_enable(bool on)
{
    s.diag_enabled = on;
}

void scr_svc_diag_poll_now(void)
{
    uint8_t op = OP_DIAG;
    (void)xQueueSend(s.q, &op, 0);
}

void scr_svc_get_c6diag(svc_c6diag_t *out)
{
    LOCK();
    *out = s.diag;
    out->age_ms = s.diag_ok_ms ? now_ms() - s.diag_ok_ms : -1;
    UNLOCK();
}

/* ---- calibration / DPT ---------------------------------------------------------------*/
static bool send_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    uint8_t f[PROTO_MAX_FRAME];
    size_t n = proto_build(cmd, scr_link_next_seq(), data, len, f, sizeof(f));
    return n > 0 && scr_link_send_bin(f, n);
}

bool scr_svc_cal_rec_get(void)
{
    return send_cmd(PROTO_CMD_DPT_REC_GET, NULL, 0);
}

bool scr_svc_cal_start_dir(void)
{
    LOCK();
    s.cal.have_cal = false;
    s.cal.cal_started_ms = now_ms();
    UNLOCK();
    bool ok = send_cmd(PROTO_CMD_DPT_ENTER, NULL, 0);
    if (ok) {
        app_state_log(SCR_LOG_WARN, "Encoder direction calibration started");
    }
    return ok;
}

bool scr_svc_cal_rec_set(int16_t full_scale, int16_t wheel_dia)
{
    svc_cal_t c;
    scr_svc_get_cal(&c);
    if (!c.have_rec) {
        return false;       /* pos/invert must come from a real record */
    }
    uint8_t d[12];
    memcpy(&d[0], c.pos, 4);
    memcpy(&d[4], c.rec_invert, 4);
    d[8] = (uint8_t)(full_scale & 0xFF);
    d[9] = (uint8_t)((uint16_t)full_scale >> 8);
    d[10] = (uint8_t)(wheel_dia & 0xFF);
    d[11] = (uint8_t)((uint16_t)wheel_dia >> 8);
    bool ok = send_cmd(PROTO_CMD_DPT_REC_SET, d, sizeof(d));
    if (ok) {
        app_state_log(SCR_LOG_INFO, "Calib write fullScale=%d wheelDia=%d", full_scale, wheel_dia);
    }
    return ok;
}

bool scr_svc_cal_rec_clear(void)
{
    bool ok = send_cmd(PROTO_CMD_DPT_REC_CLEAR, NULL, 0);
    if (ok) {
        app_state_log(SCR_LOG_WARN, "Calib record reset to defaults");
    }
    return ok;
}

bool scr_svc_clear_fault(void)
{
    bool ok = send_cmd(PROTO_CMD_CLEAR_FAULT, NULL, 0);
    if (ok) {
        app_state_log(SCR_LOG_INFO, "Clear-fault request sent");
    }
    return ok;
}

void scr_svc_get_cal(svc_cal_t *out)
{
    LOCK();
    *out = s.cal;
    UNLOCK();
}

/* ---- WS text events (websocket task, core 0) -------------------------------------------*/
static int j_arr_i(const cJSON *a, int i)
{
    const cJSON *v = cJSON_GetArrayItem(a, i);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

void scr_svc_on_ws_json(const char *type, const void *cjson_root)
{
    const cJSON *r = (const cJSON *)cjson_root;
    if (strcmp(type, "cal") == 0) {
        const cJSON *inv = cJSON_GetObjectItem(r, "invert");
        const cJSON *dl = cJSON_GetObjectItem(r, "delta");
        const cJSON *sv = cJSON_GetObjectItem(r, "saved");
        LOCK();
        s.cal.have_cal = true;
        s.cal.cal_started_ms = 0;
        s.cal.status = (uint8_t)j_u32(r, "status");
        s.cal.saved = cJSON_IsNumber(sv) ? sv->valueint : -1;
        for (int i = 0; i < 4; i++) {
            s.cal.invert[i] = (int8_t)j_arr_i(inv, i);
            s.cal.delta[i] = (int32_t)j_arr_i(dl, i);
        }
        UNLOCK();
        app_state_log(SCR_LOG_INFO, "Calib result status=%u", (unsigned)j_u32(r, "status"));
        (void)scr_svc_cal_rec_get();        /* refresh the stored record */
    } else if (strcmp(type, "rec") == 0) {
        const cJSON *pos = cJSON_GetObjectItem(r, "pos");
        const cJSON *inv = cJSON_GetObjectItem(r, "invert");
        const cJSON *fs = cJSON_GetObjectItem(r, "fullScale");
        const cJSON *wd = cJSON_GetObjectItem(r, "wheelDia");
        const cJSON *ck = cJSON_GetObjectItem(r, "crcOk");
        LOCK();
        s.cal.have_rec = true;
        s.cal.rec_ms = now_ms();
        s.cal.rec_ver = (uint8_t)j_u32(r, "ver");
        s.cal.rec_src = (uint8_t)j_u32(r, "src");
        s.cal.rec_crc_ok = (uint8_t)(cJSON_IsTrue(ck) ? 1 : (cJSON_IsNumber(ck) ? ck->valueint : 0));
        for (int i = 0; i < 4; i++) {
            s.cal.pos[i] = (uint8_t)j_arr_i(pos, i);
            s.cal.rec_invert[i] = (int8_t)j_arr_i(inv, i);
        }
        s.cal.full_scale = (int16_t)(cJSON_IsNumber(fs) ? fs->valueint : 0);
        s.cal.wheel_dia = (int16_t)(cJSON_IsNumber(wd) ? wd->valueint : 0);
        UNLOCK();
    } else if (strcmp(type, "jogcnt") == 0) {
        const cJSON *d = cJSON_GetObjectItem(r, "d");
        LOCK();
        s.cal.have_jog = true;
        s.cal.jog_on = j_u32(r, "on") != 0;
        s.cal.jog_ms = now_ms();
        for (int i = 0; i < 4; i++) {
            s.cal.jog_d[i] = (int32_t)j_arr_i(d, i);
        }
        UNLOCK();
    } else if (strcmp(type, "otastatus") == 0) {
        uint32_t stv = j_u32(r, "state");
        uint32_t pct = j_u32(r, "pct");
        LOCK();
        s.ota.tc_pct = (uint8_t)(pct > 100 ? 100 : pct);
        if (s.ota.target == SVC_FW_TC &&
            (s.ota.phase == SVC_OTA_SEND || s.ota.phase == SVC_OTA_WAIT_TC)) {
            if (stv == PROTO_OTA_STATE_DONE) {
                s.ota.phase = SVC_OTA_DONE;
                strlcpy(s.ota.msg, "TC275 image written and verified", sizeof(s.ota.msg));
            } else if (stv == PROTO_OTA_STATE_FAILED) {
                s.ota.phase = SVC_OTA_FAILED;
                strlcpy(s.ota.msg, "TC275 reported write failure", sizeof(s.ota.msg));
            }
        }
        UNLOCK();
    } else if (strcmp(type, "otaswap") == 0) {
        LOCK();
        if (s.ota.target == SVC_FW_TC && s.ota.phase != SVC_OTA_FAILED) {
            s.ota.phase = SVC_OTA_DONE;
            strlcpy(s.ota.msg, "TC275 is rebooting into the new image", sizeof(s.ota.msg));
        }
        UNLOCK();
        app_state_log(SCR_LOG_INFO, "TC275 OTA swap");
    } else if (strcmp(type, "otaerror") == 0) {
        LOCK();
        if (s.ota.target == SVC_FW_TC && s.ota.phase != SVC_OTA_IDLE) {
            s.ota.phase = SVC_OTA_FAILED;
            strlcpy(s.ota.msg, "relay aborted: TC275 stopped acknowledging", sizeof(s.ota.msg));
        }
        UNLOCK();
        app_state_log(SCR_LOG_WARN, "TC275 OTA relay error");
    }
}

void scr_svc_on_ws_down(void)
{
    LOCK();
    s.cal.cal_started_ms = 0;
    UNLOCK();
}

/* ---- task ------------------------------------------------------------------------------*/
static void svc_task(void *arg)
{
    stage_scan_one(SVC_FW_C6);
    stage_scan_one(SVC_FW_TC);
    int64_t last_diag = 0;

    for (;;) {
        uint8_t op = 0;
        (void)xQueueReceive(s.q, &op, pdMS_TO_TICKS(250));
        switch (op) {
            case OP_RESCAN:
                stage_scan_one(SVC_FW_C6);
                stage_scan_one(SVC_FW_TC);
                break;
            case OP_OTA:
                ota_run();
                break;
            case OP_DIAG:
                diag_poll();
                last_diag = now_ms();
                break;
            default:
                break;
        }
        if (s.diag_enabled && now_ms() - last_diag >= DIAG_PERIOD_MS) {
            scr_state_t st;
            app_state_snapshot(&st);
            if (st.conn == SCR_CONN_CONNECTED) {
                diag_poll();
            } else {
                LOCK();
                s.diag.valid = false;
                UNLOCK();
            }
            last_diag = now_ms();
        }
    }
}

void scr_svc_start(void)
{
    memset(&s, 0, sizeof(s));
    s.mtx = xSemaphoreCreateMutex();
    s.q = xQueueCreate(4, sizeof(uint8_t));
    s.cal.saved = -1;
    s.part[SVC_FW_C6] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "fw_c6");
    s.part[SVC_FW_TC] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "fw_tc");
    for (int i = 0; i < SVC_FW_COUNT; i++) {
        strlcpy(s.stage[i].why, "scanning", sizeof(s.stage[i].why));
        s.stage[i].state = SVC_STAGE_BAD;
    }
    /* doc/08 §2: service work is core 0, low priority, never on the UI core */
    if (xTaskCreatePinnedToCore(svc_task, "scr_svc", SVC_STACK, NULL, SVC_PRIO, NULL,
                                SVC_CORE) != pdPASS) {
        app_state_log(SCR_LOG_CRIT, "svc task create failed");
        return;
    }
    ESP_LOGI(TAG, "svc on core %d prio %d; fw_c6=%s fw_tc=%s", SVC_CORE, SVC_PRIO,
             s.part[SVC_FW_C6] ? "ok" : "MISSING", s.part[SVC_FW_TC] ? "ok" : "MISSING");
}
