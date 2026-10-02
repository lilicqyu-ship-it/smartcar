/*
 * scr_cam.c - Camera WS client + JPEG decode + latest-only slot ring
 *
 * Mirrors scr_link's proven patterns (event-driven start, silence watchdog,
 * try-then-recover reconnect) for the video plane only.  Nothing in this file
 * may touch proto frames, the control WS handle, or LVGL objects
 * (doc/SmartCar_S3Remote_详细设计说明书_V1.0.md §16 module boundaries).
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "jpeg_decoder.h"
#include "cJSON.h"
#include "sdkconfig.h"

#include "app_state.h"
#include "scr_cam.h"
#include "scr_link.h"
#include "scr_settings.h"
#include "proto/cam_frame.h"
#include "proto/vision.h"

static const char *TAG = "scr_cam";

#if CONFIG_SCR_CAM_WS_ENABLE

#define MON_PERIOD_MS       250
#define PAUSED_PING_MS      4000   /* text ping while paused: keeps the gateway
                                    * recv watchdog off and samples plane RTT  */
#define ASM_CAP             (CAM_HEADER_LEN + CONFIG_SCR_CAM_MAX_JPEG_BYTES)
#define WS_RX_BUF           4096    /* client rx buffer; larger frames arrive
                                     * as payload_offset fragments           */

#if CONFIG_SCR_CAM_MAX_JPEG_BYTES != 65536
/* cam_frame_parse enforces the PROTOCOL ceiling (CAM_JPEG_LEN_MAX); the
 * reassembly buffer must at least accept everything parse can deem valid. */
#undef ASM_CAP
#define ASM_CAP (CAM_HEADER_LEN + ((CONFIG_SCR_CAM_MAX_JPEG_BYTES > CAM_JPEG_LEN_MAX) ? \
                                   CONFIG_SCR_CAM_MAX_JPEG_BYTES : CAM_JPEG_LEN_MAX))
#endif

#define MAX_SLOT_PX         (640u * 480u)   /* decode budget: 614 KB RGB565    */

typedef struct {
    uint8_t  *rgb;          /* PSRAM, w*h*2 bytes                              */
    uint32_t  seq;          /* frame seq this slot holds (0 = empty)           */
    uint16_t  w, h;
} slot_t;

typedef struct {
    esp_websocket_client_handle_t ws;
    volatile bool running;            /* client task alive + WS up             */
    volatile bool want_stream;        /* a camera-facing page is visible       */
    volatile bool sub_sent;           /* subscribe delivered (per CONNECTED)   */
    volatile int64_t last_rx_ms;      /* any Camera WS traffic                 */
    volatile int64_t last_frame_ms;   /* any accepted video frame              */
    volatile int64_t last_ping_ms;    /* paused-plane keepalive cadence        */
    bool     ping_pending;            /* one RTT sample in flight              */
    int64_t  ping_sent_ms;
    uint16_t last_rtt_ms;             /* last pong round trip (trace + DIAG)   */

    uint8_t *asm_buf;                 /* PSRAM: WS fragment reassembly         */
    size_t  asm_got;
    bool    asm_overflow;

    /* Latest-only pending frame, depth 1, double buffered: the WS task fills
     * pend[pend_idx] while the decoder swaps ownership under pend_mtx and
     * decodes the other buffer, so esp_jpeg_decode never reads bytes being written. */
    uint8_t *pend[2];
    int      pend_idx;
    size_t   pend_len;                /* bytes in pend[pend_idx], 0 = empty    */
    cam_frame_hdr_t pend_hdr;         /* parsed header of that frame           */
    SemaphoreHandle_t pend_mtx;
    SemaphoreHandle_t decode_sem;     /* binary, given on every new pend       */

    slot_t  slot[CONFIG_SCR_CAM_SLOTS];
    uint16_t slot_w, slot_h;          /* dims the ring was allocated for       */
    int     display_idx;              /* -1 none; LVGL-owned, decoder avoids   */
    int     newest_idx;               /* -1 none; published, unretrieved       */
    SemaphoreHandle_t slot_mtx;       /* held only for index swaps             */

    /* accounting (owned here, mirrored to app_state at 1 Hz) */
    uint32_t seq_last;
    bool     seq_hist;
    uint32_t cnt_drop, cnt_frame_err, cnt_decode_err;
    uint32_t win_frames;
    uint16_t win_decode_ms_max;
    uint16_t e2e_ms;
    int64_t  e2e_base_ms;             /* local_ms - gateway_ts, learned/frame  */
    int64_t  stat_tick_ms;

    /* esp_jpeg 1.x is stateless (TJpgDec); it wants a static scratch pad */
    uint8_t *jpeg_ws;                 /* working buffer, NULL = per-call alloc  */
    size_t   jpeg_ws_size;
    bool    degraded;                 /* PSRAM alloc failed: paused, no retry  */
} cam_t;

static cam_t s_cam;

/* One retired generation of slot buffers.  The LVGL task borrows a pointer
 * out of the live ring; when a resolution change re-creates the ring we never
 * free the borrowed buffers, we park them here until the NEXT re-creation
 * (bounded to one generation, no dangling read). */
static slot_t s_retire[CONFIG_SCR_CAM_SLOTS];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---- slot ring ---------------------------------------------------------------*/
static void slots_release(slot_t *ring)
{
    for (int i = 0; i < CONFIG_SCR_CAM_SLOTS; i++) {
        heap_caps_free(ring[i].rgb);
        ring[i].rgb = NULL;
        ring[i].seq = 0;
        ring[i].w = 0;
        ring[i].h = 0;
    }
}

/* Allocate the ring for w x h.  Returns false when PSRAM cannot cover it
 * (design doc 10: fail -> degrade, never crash, never touch other planes). */
static bool slots_ensure(uint16_t w, uint16_t h)
{
    if (s_cam.slot_w == w && s_cam.slot_h == h && s_cam.slot[0].rgb != NULL) {
        return true;
    }
    size_t need = (size_t)w * h * 2u;
    if (need == 0 || (size_t)w * h > MAX_SLOT_PX) {
        return false;
    }
    xSemaphoreTake(s_cam.slot_mtx, portMAX_DELAY);
    slots_release(s_retire);            /* previous generation now unreachable */
    for (int i = 0; i < CONFIG_SCR_CAM_SLOTS; i++) {
        s_retire[i] = s_cam.slot[i];
        s_cam.slot[i].rgb = NULL;
        s_cam.slot[i].seq = 0;
    }
    s_cam.display_idx = -1;
    s_cam.newest_idx = -1;

    bool ok = true;
    for (int i = 0; i < CONFIG_SCR_CAM_SLOTS && ok; i++) {
        s_cam.slot[i].rgb = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        if (s_cam.slot[i].rgb == NULL) {
            ok = false;
        }
        s_cam.slot[i].w = w;
        s_cam.slot[i].h = h;
    }
    if (ok) {
        s_cam.slot_w = w;
        s_cam.slot_h = h;
    } else {
        slots_release(s_cam.slot);
        s_cam.slot_w = 0;
        s_cam.slot_h = 0;
    }
    xSemaphoreGive(s_cam.slot_mtx);

    if (!ok && !s_cam.degraded) {
        s_cam.degraded = true;
        app_state_log(SCR_LOG_WARN, "CAM DEGRADED: no PSRAM for %ux%u", w, h);
    }
    return ok;
}

const uint8_t *scr_cam_display_acquire(uint16_t *w, uint16_t *h, uint32_t *seq)
{
    xSemaphoreTake(s_cam.slot_mtx, portMAX_DELAY);
    int n = s_cam.newest_idx;
    if (n < 0 || s_cam.slot[n].rgb == NULL) {
        xSemaphoreGive(s_cam.slot_mtx);
        return NULL;
    }
    /* retire the previously displayed slot, adopt the newest */
    s_cam.display_idx = n;
    s_cam.newest_idx = -1;
    const uint8_t *buf = s_cam.slot[n].rgb;
    if (w)   { *w = s_cam.slot[n].w; }
    if (h)   { *h = s_cam.slot[n].h; }
    if (seq) { *seq = s_cam.slot[n].seq; }
    xSemaphoreGive(s_cam.slot_mtx);
    return buf;
}

/* ---- decode ------------------------------------------------------------------*/
/* buf points at a whole reassembled WS frame (header + JPEG) that the caller
 * exclusively owns; hdr is its parsed header. */
static void decode_frame(const uint8_t *buf, const cam_frame_hdr_t *hdr)
{
    if (!slots_ensure(hdr->width, hdr->height)) {
        return;                               /* degraded: frame dropped */
    }

    const uint8_t *jpg = buf + CAM_OFF_PAYLOAD;
    int64_t t0 = now_ms();

    /* decode straight into a slot the LVGL is NOT displaying */
    xSemaphoreTake(s_cam.slot_mtx, portMAX_DELAY);
    int tgt = -1;
    for (int i = 0; i < CONFIG_SCR_CAM_SLOTS; i++) {
        if (i != s_cam.display_idx && i != s_cam.newest_idx &&
            s_cam.slot[i].rgb != NULL) {
            tgt = i;
            break;
        }
    }
    if (tgt < 0) {
        /* only the un-acquired newest is spare: recycle it (latest wins, the
         * stale content was about to be superseded anyway) */
        for (int i = 0; i < CONFIG_SCR_CAM_SLOTS; i++) {
            if (i != s_cam.display_idx && s_cam.slot[i].rgb != NULL) {
                tgt = i;
                break;
            }
        }
    }
    if (tgt < 0) {
        /* every spare slot is occupied by in-flight content: latest-only drop */
        s_cam.cnt_drop++;
        xSemaphoreGive(s_cam.slot_mtx);
        return;
    }
    uint8_t *out = s_cam.slot[tgt].rgb;
    size_t cap = (size_t)hdr->width * hdr->height * 2u;
    xSemaphoreGive(s_cam.slot_mtx);

    esp_jpeg_image_cfg_t cfg = {
        .indata       = (uint8_t *)jpg,
        .indata_size  = hdr->jpeg_len,
        .outbuf       = out,
        .outbuf_size  = (uint32_t)cap,
        .out_format   = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale    = JPEG_IMAGE_SCALE_0,
        /* TJpgDec emits big-endian RGB565; LVGL wants native LE words */
        .flags.swap_color_bytes = 1,
        .advanced.working_buffer      = s_cam.jpeg_ws,
        .advanced.working_buffer_size = s_cam.jpeg_ws_size,
    };
    esp_jpeg_image_output_t info = {0};
    esp_err_t err = esp_jpeg_decode(&cfg, &info);
    int out_size = (err == ESP_OK) ? (int)info.output_len : -1;
    int64_t dt = now_ms() - t0;

    if (err != 0 || out_size <= 0 || (size_t)out_size != cap) {
        xSemaphoreTake(s_cam.slot_mtx, portMAX_DELAY);
        s_cam.slot[tgt].seq = 0;            /* poison: never display a torn slot */
        xSemaphoreGive(s_cam.slot_mtx);
        s_cam.cnt_decode_err++;
        return;
    }

    if (dt > s_cam.win_decode_ms_max) {
        s_cam.win_decode_ms_max = (uint16_t)dt;
    }

    xSemaphoreTake(s_cam.slot_mtx, portMAX_DELAY);
    s_cam.slot[tgt].seq = hdr->seq;
    /* the old newest (not yet fetched by LVGL) is superseded: free to reuse */
    s_cam.newest_idx = tgt;
    xSemaphoreGive(s_cam.slot_mtx);
}

static void decode_task(void *arg)
{
    for (;;) {
        if (xSemaphoreTake(s_cam.decode_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        /* swap ownership of the pending buffer out of the WS task's reach */
        uint8_t *buf = NULL;
        cam_frame_hdr_t hdr;
        xSemaphoreTake(s_cam.pend_mtx, portMAX_DELAY);
        if (s_cam.pend_len > 0) {
            buf = s_cam.pend[s_cam.pend_idx];
            hdr = s_cam.pend_hdr;
            s_cam.pend_len = 0;
            s_cam.pend_idx ^= 1;
        }
        xSemaphoreGive(s_cam.pend_mtx);
        if (buf != NULL) {
            decode_frame(buf, &hdr);
        }
    }
}

/* ---- camera WS text plane (cam_hello / cam_state) ----------------------------*/
static void cam_handle_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (root == NULL) {
        return;                             /* partial text: shape-tolerant drop */
    }
    const cJSON *t     = cJSON_GetObjectItem(root, VISION_F_T);
    const cJSON *hello = cJSON_GetObjectItem(root, "hello");
    /* LLDD 8.2 hello may arrive as {"hello":...} with no "t", or tagged */
    bool is_hello = hello != NULL ||
                    (cJSON_IsString(t) &&
                     strcmp(t->valuestring, VISION_T_CAM_HELLO) == 0);

    const cJSON *sensor = cJSON_GetObjectItem(root, "sensor");
    const cJSON *pw  = cJSON_GetObjectItem(root, "w");
    const cJSON *ph  = cJSON_GetObjectItem(root, "h");

    if (is_hello || cJSON_IsString(sensor)) {
        app_state_set_cam_hello(cJSON_IsString(sensor) ? sensor->valuestring : "?",
                                (uint16_t)(cJSON_IsNumber(pw) ? pw->valuedouble : 0),
                                (uint16_t)(cJSON_IsNumber(ph) ? ph->valuedouble : 0));
        if (is_hello) {
            ESP_LOGI(TAG, "camera hello: sensor=%s %ux%u",
                     cJSON_IsString(sensor) ? sensor->valuestring : "?",
                     (unsigned)(cJSON_IsNumber(pw) ? pw->valuedouble : 0),
                     (unsigned)(cJSON_IsNumber(ph) ? ph->valuedouble : 0));
        }
        s_cam.degraded = false;             /* gateway re-announced: retry budget */
    }
    const cJSON *op = cJSON_GetObjectItem(root, "op");
    if (cJSON_IsString(op) && strcmp(op->valuestring, CAM_WS_OP_PONG_NAME) == 0 &&
        s_cam.ping_pending) {
        int64_t rtt = now_ms() - s_cam.ping_sent_ms;
        if (rtt >= 0 && rtt < 30000) {
            s_cam.last_rtt_ms = (uint16_t)rtt;
            app_state_set_cam_rtt((uint16_t)rtt);
            /* Serial-visible proof that the text plane round-trips without the
             * CAMERA page being open - the bench verdict needs no UI tap. */
            ESP_LOGI(TAG, "camera pong: rtt=%lldms", (long long)rtt);
        }
        s_cam.ping_pending = false;
    }
    /* "sd"/"rec" fields are ignored: the S3-CAM has no MicroSD (SD feature
     * dropped from LLDD V1.0), no storage state reaches the UI. */
    cJSON_Delete(root);
}

/* ---- camera WS binary plane: reassembly + parse + pend -----------------------*/
static void cam_handle_binary(esp_websocket_event_data_t *ev)
{
    /* one WS frame == one header + one JPEG (design G-2); a frame larger than
     * the client buffer (WS_RX_BUF = 4096) arrives as several events, each
     * carrying ev->data_len bytes at ev->payload_offset - NOT payload_len,
     * which is the WHOLE frame's length (see scr_link's proven RX) */
    size_t clen  = (size_t)ev->data_len;
    size_t total = (size_t)ev->payload_offset + clen;

    if (total > ASM_CAP || s_cam.asm_buf == NULL) {
        if (!s_cam.asm_overflow && total > ASM_CAP) {
            s_cam.asm_overflow = true;
            s_cam.cnt_frame_err++;
        }
        return;
    }
    memcpy(s_cam.asm_buf + (size_t)ev->payload_offset,
           ev->data_ptr, clen);
    if (!ev->fin) {
        return;
    }
    s_cam.asm_overflow = false;

    cam_frame_hdr_t hdr;
    cam_rx_ev_t r = cam_frame_parse(s_cam.asm_buf, total, &hdr);
    if (r != CAM_RX_OK) {
        s_cam.cnt_frame_err++;
        return;
    }
    uint32_t dropped = 0;
    cam_seq_st_t st = cam_seq_note(s_cam.seq_last, s_cam.seq_hist, hdr.seq, &dropped);
    if (st == CAM_SEQ_STALE) {
        return;                                 /* late/reordered: keep newest */
    }
    if (st == CAM_SEQ_RESYNC) {
        s_cam.e2e_base_ms = 0;                  /* sender restarted */
    } else {
        s_cam.cnt_drop += dropped;
    }
    s_cam.seq_last = hdr.seq;
    s_cam.seq_hist = true;

    /* e2e: local receive vs gateway capture stamp */
    int64_t local = now_ms();
    int64_t est = local - (int64_t)hdr.timestamp_ms;
    if (s_cam.e2e_base_ms == 0) {
        s_cam.e2e_base_ms = est;
    }
    int64_t e2e = est - s_cam.e2e_base_ms;
    if (e2e >= 0 && e2e < 30000) {
        s_cam.e2e_ms = (uint16_t)((s_cam.e2e_ms + e2e) / 2);
    }

    xSemaphoreTake(s_cam.pend_mtx, portMAX_DELAY);
    if (s_cam.pend_len != 0) {
        s_cam.cnt_drop++;                       /* overwrite undecoded pend     */
    }
    memcpy(s_cam.pend[s_cam.pend_idx], s_cam.asm_buf, total);
    s_cam.pend_hdr = hdr;
    s_cam.pend_len = total;
    xSemaphoreGive(s_cam.pend_mtx);

    s_cam.last_frame_ms = local;
    s_cam.win_frames++;
    app_state_note_cam_frame();
    xSemaphoreGive(s_cam.decode_sem);
}

static void cam_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            s_cam.running = true;
            s_cam.sub_sent = false;
            s_cam.last_rx_ms = now_ms();
            s_cam.seq_hist = false;
            s_cam.e2e_base_ms = 0;
            app_state_set_cam_conn(SCR_CAM_CONNECTED);
            ESP_LOGI(TAG, "Camera WS connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            s_cam.running = false;
            s_cam.sub_sent = false;
            app_state_set_cam_conn(SCR_CAM_CONNECTING);
            ESP_LOGW(TAG, "Camera WS disconnected");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (ev->data_len <= 0) {
                break;
            }
            s_cam.last_rx_ms = now_ms();
            if (ev->op_code == 0x02) {
                cam_handle_binary(ev);
            } else if (ev->op_code == 0x01) {
                cam_handle_text(ev->data_ptr, ev->data_len);
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            s_cam.running = false;
            app_state_set_cam_conn(SCR_CAM_CONNECTING);
            break;
        default:
            break;
    }
}

/* ---- text sends (Camera WS plane) --------------------------------------------*/
static bool cam_send_text(const char *txt)
{
    if (s_cam.ws == NULL || !esp_websocket_client_is_connected(s_cam.ws)) {
        return false;
    }
    return esp_websocket_client_send_text(s_cam.ws, txt, (int)strlen(txt),
                                          pdMS_TO_TICKS(150)) > 0;
}

bool scr_cam_cmd_profile(const char *name)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"op\":\"profile\",\"name\":\"%s\"}", name);
    return cam_send_text(buf);
}

static void cam_subscribe_now(void)
{
    if (!cam_send_text(CAM_WS_SUBSCRIBE)) {
        return;
    }
    s_cam.sub_sent = true;
    app_state_set_cam_subscribed(true);
    /* Announce the size this handset wants instead of accepting whatever the
     * gateway boots at: the sensor is one global setting, so a silent subscribe
     * left Kconfig saying REMOTE_PREVIEW while the air carried the gateway's VGA.
     * The CAMERA page's radio mirrors the returned dims (spec 106), so without
     * this command it could only ever show the gateway's choice. */
#if CONFIG_SCR_CAM_PROFILE_REMOTE
    ESP_LOGI(TAG, "subscribe sent (%s)", CAM_PROFILE_REMOTE);
    scr_cam_cmd_profile(CAM_PROFILE_REMOTE);
#else
    ESP_LOGI(TAG, "subscribe sent (%s)", CAM_PROFILE_WEB);
    scr_cam_cmd_profile(CAM_PROFILE_WEB);
#endif
}

/* ---- monitor task -------------------------------------------------------------*/
static void cam_ws_start(void)
{
    scr_settings_t set;
    scr_settings_get(&set);

    char uri[192];
    /* the gateway serves /ws/camera on its own stream httpd (s3_camera,
     * default :81), NOT on the control-plane :80 - doc/21 §3.3 */
    if (set.token[0] != '\0') {
        snprintf(uri, sizeof(uri), "ws://%s:%u/ws/camera?token=%s",
                 CONFIG_SCR_C6_IP, (unsigned)CONFIG_SCR_CAM_STREAM_PORT, set.token);
    } else {
        snprintf(uri, sizeof(uri), "ws://%s:%u/ws/camera",
                 CONFIG_SCR_C6_IP, (unsigned)CONFIG_SCR_CAM_STREAM_PORT);
    }

    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .buffer_size = WS_RX_BUF,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 3000,
        /* reconnection is owned by cam_monitor_task, not the client: it must
         * see the control-link state before dialing (design doc 10) */
        .disable_auto_reconnect = true,
        .task_stack = 6144,
        .task_prio = 5,
        .task_core_id_set = true,
        .task_core_id = 0,
    };
    s_cam.ws = esp_websocket_client_init(&cfg);
    if (s_cam.ws == NULL) {
        app_state_log(SCR_LOG_WARN, "Camera WS init failed");
        app_state_set_cam_conn(SCR_CAM_IDLE);
        return;
    }
    esp_websocket_register_events(s_cam.ws, WEBSOCKET_EVENT_ANY, cam_ws_event, NULL);
    esp_websocket_client_start(s_cam.ws);
    app_state_set_cam_conn(SCR_CAM_CONNECTING);
}

static void cam_ws_stop(void)
{
    if (s_cam.ws) {
        esp_websocket_client_stop(s_cam.ws);
        esp_websocket_client_destroy(s_cam.ws);
        s_cam.ws = NULL;
    }
    s_cam.running = false;
    s_cam.sub_sent = false;
}

static void cam_ws_restart(void)
{
    ESP_LOGW(TAG, "Camera WS restart");
    cam_ws_stop();
    app_state_set_cam_conn(SCR_CAM_CONNECTING);
    cam_ws_start();
}

static void push_stats_1hz(void)
{
    static int64_t last_ms;
    int64_t now = now_ms();
    if (last_ms == 0) {
        last_ms = now;
    }
    int64_t dt = now - last_ms;
    if (dt < 900) {
        return;
    }
    uint8_t fps_x10 = 0;
    uint32_t win = s_cam.win_frames;
    if (s_cam.last_frame_ms != 0) {
        fps_x10 = (uint8_t)((s_cam.win_frames * 10000u + dt / 2) / (uint64_t)dt);
        if (fps_x10 > CONFIG_SCR_CAM_MAX_FPS * 10u) {
            fps_x10 = CONFIG_SCR_CAM_MAX_FPS * 10u;
        }
    }
    app_state_set_cam_stats(fps_x10, s_cam.win_decode_ms_max, s_cam.e2e_ms);
    app_state_set_cam_counters(s_cam.seq_last, s_cam.cnt_drop,
                               s_cam.cnt_frame_err, s_cam.cnt_decode_err);

    /* bench trace: the video plane has no on-screen counters, so without this
     * line a stalled pipeline is indistinguishable from a paused page. Printed
     * while a camera page is up OR while frames are arriving (the gateway
     * pushes from connect until pause lands - doc/19 §5.4). */
    static uint8_t trace_div;
    if (++trace_div % 2 == 0 && (s_cam.want_stream || win != 0u)) {
        ESP_LOGI(TAG, "cam want=%d %u.%ufps win=%u seq=%u drop=%u ferr=%u derr=%u "
                      "dec=%ums e2e=%ums rtt=%ums",
                 s_cam.want_stream ? 1 : 0,
                 (unsigned)(fps_x10 / 10u), (unsigned)(fps_x10 % 10u),
                 (unsigned)win, (unsigned)s_cam.seq_last,
                 (unsigned)s_cam.cnt_drop, (unsigned)s_cam.cnt_frame_err,
                 (unsigned)s_cam.cnt_decode_err,
                 (unsigned)s_cam.win_decode_ms_max, (unsigned)s_cam.e2e_ms,
                 (unsigned)s_cam.last_rtt_ms);
    }

    s_cam.win_frames = 0;
    s_cam.win_decode_ms_max = 0;
    last_ms = now;

    /* consecutive-frame gap while subscribed: force one resubscribe
     * (design doc 10: pause -> resubscribe; the next streak waits for the user) */
    if (s_cam.want_stream && s_cam.last_frame_ms != 0 &&
        now - s_cam.last_frame_ms > CONFIG_SCR_CAM_FRAME_TIMEOUT_MS) {
        if (s_cam.sub_sent) {
            ESP_LOGW(TAG, "no frames while subscribed: forcing resubscribe");
            s_cam.sub_sent = false;       /* monitor retries next tick         */
        }
    }
}

/* The single connection owner: the video plane only exists while the control
 * plane has a live gateway session (design doc 10 - Control WS 断开则 Camera WS
 * 一并断开), every dial/teardown decision and every watchdog lives here.
 * auto_reconnect is off on purpose: dialing must be gated, not blind. */
static void cam_monitor_task(void *arg)
{
    int64_t last_dial_ms = 0;      /* 3 s reconnect cadence, shared */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MON_PERIOD_MS));
        int64_t now = now_ms();

        push_stats_1hz();

        scr_state_t st;
        app_state_snapshot(&st);
        bool ctrl_up = (st.conn == SCR_CONN_CONNECTED);

        if (!scr_link_wifi_up() || !ctrl_up) {
            /* gateway session gone: tear the video plane down instead of
             * storm-retrying a dead host; CAM badge shows OFFLINE */
            if (s_cam.ws != NULL) {
                cam_ws_stop();
                app_state_set_cam_conn(SCR_CAM_IDLE);
            }
            continue;
        }

        if (s_cam.ws == NULL || !s_cam.running) {
            /* cold start, or a DISCONNECTED/ERROR edge: redial on a fixed
             * cadence; cam_ws_start re-reads the pair token every time */
            if (now - last_dial_ms >= 3000) {
                last_dial_ms = now;
                if (s_cam.ws == NULL) {
                    cam_ws_start();
                } else {
                    cam_ws_restart();
                }
            }
            continue;
        }

        /* subscription (re)send is idempotent and rate-limited by this tick */
        if (s_cam.want_stream && !s_cam.sub_sent && !s_cam.degraded) {
            cam_subscribe_now();
            s_cam.last_ping_ms = now;
        } else if (!s_cam.want_stream &&
                   now - s_cam.last_ping_ms >= PAUSED_PING_MS) {
            /* paused keepalive: it is NOT a lease - esp_http_server only runs a
             * WS handler when the socket is readable, so an idle session is never
             * reclaimed by recv_wait_timeout (that is just SO_RCVTIMEO).  What the
             * ping buys is the camera plane's RTT sample for DIAG and proof that
             * this second connection is still bidirectional. */
            s_cam.last_ping_ms = now;
            if (cam_send_text(CAM_WS_PING)) {
                s_cam.ping_sent_ms = now;
                s_cam.ping_pending = true;
            }
        }

        /* silence watchdog only judges a connection that SHOULD be streaming:
         * a paused page is legitimately idle and must never be torn down */
        if (s_cam.want_stream &&
            now - s_cam.last_rx_ms > CONFIG_SCR_CAM_RX_WATCHDOG_MS) {
            last_dial_ms = now;
            cam_ws_restart();
        }
    }
}

/* ---- lifecycle -----------------------------------------------------------------*/
void scr_cam_page_enter(void)
{
    s_cam.want_stream = true;
    if (s_cam.running && !s_cam.sub_sent) {
        cam_subscribe_now();
    } else if (s_cam.running) {
        app_state_set_cam_subscribed(true);   /* already subscribed this session */
    }
}

void scr_cam_page_leave(void)
{
    if (!s_cam.want_stream) {
        return;
    }
    s_cam.want_stream = false;
    if (s_cam.running && s_cam.sub_sent) {
        cam_send_text(CAM_WS_PAUSE);
        s_cam.sub_sent = false;             /* next enter must re-subscribe */
    }
    app_state_set_cam_subscribed(false);
}

void scr_cam_start(void)
{
    memset(&s_cam, 0, sizeof(s_cam));
    s_cam.display_idx = -1;
    s_cam.newest_idx = -1;
    s_cam.pend_mtx = xSemaphoreCreateMutex();
    s_cam.slot_mtx = xSemaphoreCreateMutex();
    s_cam.decode_sem = xSemaphoreCreateBinary();

    s_cam.asm_buf = heap_caps_malloc(ASM_CAP, MALLOC_CAP_SPIRAM);
    s_cam.pend[0] = heap_caps_malloc(ASM_CAP, MALLOC_CAP_SPIRAM);
    s_cam.pend[1] = heap_caps_malloc(ASM_CAP, MALLOC_CAP_SPIRAM);
    if (s_cam.asm_buf == NULL || s_cam.pend[0] == NULL || s_cam.pend[1] == NULL) {
        app_state_log(SCR_LOG_WARN, "CAM DEGRADED: reassembly alloc failed");
        s_cam.degraded = true;
        return;
    }

    /* TJpgDec (esp_jpeg 1.x) needs a non-psram scratch pad; 8 KB covers the
     * default 32-bit-fastdecode configuration.  NULL would fall back to a
     * per-call internal allocation - avoid that at stream rate. */
    s_cam.jpeg_ws_size = 8192;
    s_cam.jpeg_ws = heap_caps_malloc(s_cam.jpeg_ws_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_cam.jpeg_ws == NULL) {
        s_cam.jpeg_ws = heap_caps_malloc(s_cam.jpeg_ws_size, MALLOC_CAP_8BIT);
    }
    if (s_cam.jpeg_ws == NULL) {
        app_state_log(SCR_LOG_WARN, "CAM: jpeg working buffer alloc failed");
        s_cam.degraded = true;
        return;
    }

    if (xTaskCreatePinnedToCore(cam_monitor_task, "scr_cam", 6144, NULL, 4, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(decode_task, "cam_decode", 8192, NULL, 4, NULL, 0) != pdPASS) {
        app_state_log(SCR_LOG_CRIT, "cam task create failed");
        return;
    }
    ESP_LOGI(TAG, "camera plane up (max %u KB frame)",
             (unsigned)(ASM_CAP / 1024));
}

#else /* !CONFIG_SCR_CAM_WS_ENABLE */

void scr_cam_start(void) { }
void scr_cam_page_enter(void) { }
void scr_cam_page_leave(void) { }
bool scr_cam_cmd_profile(const char *name) { (void)name; return false; }
const uint8_t *scr_cam_display_acquire(uint16_t *w, uint16_t *h, uint32_t *seq)
{
    (void)w; (void)h; (void)seq;
    return NULL;
}

#endif /* CONFIG_SCR_CAM_WS_ENABLE */
