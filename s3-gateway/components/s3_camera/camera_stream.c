/*
 * camera_stream.c - OV5640 capture + MJPEG stream server (s3_camera)
 *
 * Ported from the bring-up bench app: the Freenove ESP32-S3-WROOM CAM is
 * pin-compatible with CAMERA_MODEL_ESP32S3_EYE, JPEG frames straight out of
 * the sensor (no encode path on the S3), delivered as multipart/x-mixed-replace
 * on /stream and as text-plane-gated binary frames on /ws/camera.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/tcp.h>

#include "driver/i2c_master.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "camera_stream.h"
#include "cam_frame.h"
#include "vision.h"

static const char *TAG = "cam";

/* Freenove ESP32-S3-WROOM CAM == CAMERA_MODEL_ESP32S3_EYE.  The camera owns
 * GPIO4..18; native USB (19/20), flash (26..32) and octal PSRAM (33..37) are
 * off limits, which is why the LINK moved to 39..42 + IRQ 2 (sdkconfig.defaults). */
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5
#define CAM_PIN_Y9     16
#define CAM_PIN_Y8     17
#define CAM_PIN_Y7     18
#define CAM_PIN_Y6     12
#define CAM_PIN_Y5     10
#define CAM_PIN_Y4      8
#define CAM_PIN_Y3      9
#define CAM_PIN_Y2     11
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK   13

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
/* boundary + part header as one string: two socket writes per frame instead of
 * three, so the ~70-byte header never travels as a segment of its own */
static const char STREAM_MJPEG_HEAD[]  = "\r\n--" PART_BOUNDARY "\r\n"
                                         "Content-Type: image/jpeg\r\n"
                                         "Content-Length: %u\r\n\r\n";

static volatile bool         s_ready;
static volatile unsigned     s_clients;
static char                  s_sensor[16];

/* diag counters (详细设计说明书 18.2): fps is measured over a rolling window by
 * the streaming task, the rest are cumulative since boot */
#define FPS_WINDOW_US 500000
static volatile uint32_t     s_fps_x10;
static volatile uint32_t     s_frames;
static volatile uint32_t     s_grab_fail;
static volatile uint16_t     s_width;
static volatile uint16_t     s_height;
static volatile uint32_t     s_seq;    /* monotonic across viewers, per 表 14 SEQ */

/* Stall = the socket write for one frame outlived 2.5 frame periods (100 ms at
 * 25 fps), i.e. the viewer - not the sensor - is the bottleneck and
 * CAMERA_GRAB_LATEST is already dropping frames on its way in.  Cumulative
 * since boot so the DIAG page can answer "did it stutter, and how badly"
 * without anyone having to watch the picture. */
#define STALL_SEND_MS 100
static volatile uint32_t     s_stalls;
static volatile uint32_t     s_worst_send_ms;

/* /ws/camera is single-session too, and its frames are dropped rather than
 * delayed while unsubscribed - counted here so cam_state can say so (表 17) */
static volatile uint32_t     s_ws_dropped;

/* esp32-camera keeps its framesize→dimensions table private, and cam_hello /
 * cam_state are composed before any frame has been grabbed - so the sizes the
 * Kconfig choice and 表 8 profiles can actually select are spelled out here. */
static void frame_size_dims(framesize_t size, uint16_t *w, uint16_t *h)
{
    switch (size) {
    case FRAMESIZE_QVGA: *w = 320;  *h = 240;  break;
    case FRAMESIZE_HVGA: *w = 480;  *h = 320;  break;
    case FRAMESIZE_VGA:  *w = 640;  *h = 480;  break;
    case FRAMESIZE_SVGA: *w = 800;  *h = 600;  break;
    case FRAMESIZE_HD:   *w = 1280; *h = 720;  break;
    default:             *w = 0;    *h = 0;    break;
    }
}

/* framesize_t numbering moves between esp32-camera releases - this vendor tree
 * inserts 128X128 and 320X320, so QVGA is 6 and VGA is 10 - and the driver
 * takes any int without complaint.  A numeric Kconfig symbol therefore drifts
 * silently (it used to say VGA and configured CIF 400x296), so the choice maps
 * to the constant by name. */
#if CONFIG_S3_CAMERA_FRAME_SIZE_QVGA
#define CAM_FRAME_SIZE FRAMESIZE_QVGA
#elif CONFIG_S3_CAMERA_FRAME_SIZE_HVGA
#define CAM_FRAME_SIZE FRAMESIZE_HVGA
#elif CONFIG_S3_CAMERA_FRAME_SIZE_VGA
#define CAM_FRAME_SIZE FRAMESIZE_VGA
#elif CONFIG_S3_CAMERA_FRAME_SIZE_SVGA
#define CAM_FRAME_SIZE FRAMESIZE_SVGA
#elif CONFIG_S3_CAMERA_FRAME_SIZE_HD
#define CAM_FRAME_SIZE FRAMESIZE_HD
#else
#error "Select a frame size under s3_camera (S3_CAMERA_FRAME_SIZE_ID)"
#endif

/* Distinguishes "cable seated wrong" from "this board has no camera at all":
 * a bare SCCB scan of the bus after esp_camera_init() failed. */
static void sccb_scan_bus(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                = 1,
        .sda_io_num              = CAM_PIN_SIOD,
        .scl_io_num              = CAM_PIN_SIOC,
        .clk_source              = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt       = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK)
    {
        ESP_LOGE(TAG, "SCCB scan skipped (GPIO4/5 busy)");
        return;
    }

    int acked = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++)
    {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK)
        {
            ESP_LOGW(TAG, "SCCB 0x%02x ACK (OV5640=0x3c, OV2640=0x30)", addr);
            acked++;
        }
    }
    if (acked == 0)
    {
        ESP_LOGE(TAG, "SCCB bus has 0 ACKs - check the ribbon (direction, seated) "
                      "or whether a camera is attached at all");
    }

    (void)i2c_del_master_bus(bus);
}

static esp_err_t camera_init(void)
{
    camera_config_t cfg = {
        .pin_pwdn     = -1,
        .pin_reset    = -1,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_Y9,
        .pin_d6       = CAM_PIN_Y8,
        .pin_d5       = CAM_PIN_Y7,
        .pin_d4       = CAM_PIN_Y6,
        .pin_d3       = CAM_PIN_Y5,
        .pin_d2       = CAM_PIN_Y4,
        .pin_d1       = CAM_PIN_Y3,
        .pin_d0       = CAM_PIN_Y2,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = CAM_FRAME_SIZE,
        .jpeg_quality = CONFIG_S3_CAMERA_JPEG_QUALITY,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        sccb_scan_bus();
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    camera_sensor_info_t *info = (s != NULL) ? esp_camera_sensor_get_info(&s->id) : NULL;
    snprintf(s_sensor, sizeof(s_sensor), "%s",
             (info != NULL && info->name != NULL) ? info->name : "unknown");
    /* The module is mounted upside-down in the chassis, so the sensor scans the
     * scene rotated 180 deg.  Mirror AND flip is the sensor-side 180 deg - it
     * fixes /stream and /ws/camera at once, and OV5640's set_image_options()
     * re-applies both bits on every framesize change, so the handset's profile
     * switch cannot lose it. */
    if (s != NULL)
    {
        int rc  = s->set_hmirror(s, 1);
        rc |= s->set_vflip(s, 1);
        if (rc != 0)
        {
            ESP_LOGW(TAG, "sensor rotate 180 failed - image will be upside-down");
        }
    }
    /* The driver keeps its size table private, and hello/cam_state are built
     * before a frame has ever been grabbed - so carry the configured dims. */
    uint16_t w = 0, h = 0;
    frame_size_dims(CAM_FRAME_SIZE, &w, &h);
    s_width  = w;
    s_height = h;
    ESP_LOGI(TAG, "sensor %s up: frame_size=%d quality=%d %ux%u",
             s_sensor, (int)CAM_FRAME_SIZE, CONFIG_S3_CAMERA_JPEG_QUALITY,
             (unsigned)s_width, (unsigned)s_height);
    s_ready = true;
    return ESP_OK;
}

/* One viewer at a time: two tasks pulling from the same 2-frame queue each
 * drop half the frames and the second one adds latency to the first.  The
 * pump is shared by both sinks (multipart MJPEG for the browser page, binary
 * WebSocket frames for the Remote handset); CAMERA_GRAB_LATEST is the
 * backpressure, so a sink that cannot keep up gets the newest frame and the
 * stale ones never leave the driver. */
typedef esp_err_t (*frame_sink_t)(httpd_req_t *req, const camera_fb_t *fb,
                                  uint32_t seq, void *ud);

/* Take the single viewer slot.  Returns false (having answered 500/503) when
 * the sensor is absent or another viewer holds the channel. */
static bool viewer_attach(httpd_req_t *req, const char *what, const char *content_type)
{
    if (!s_ready)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera unavailable");
        return false;
    }
    if (s_clients >= 1u)
    {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        (void)httpd_resp_sendstr(req, "stream busy - one viewer at a time");
        return false;
    }
    s_clients++;
    ESP_LOGI(TAG, "%s viewer attached", what);

    if (content_type != NULL)
    {
        httpd_resp_set_type(req, content_type);
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
    return true;
}

/* Grab-return-send loop until the sink fails or the client goes away.  Runs on
 * whichever task owns the session; frame stats are for /api/diag (18.2). */
static esp_err_t frame_pump(httpd_req_t *req, frame_sink_t sink, void *ud, const char *what)
{
    uint32_t frames     = 0;
    uint32_t win_frames = 0;
    size_t   bytes_sent = 0;
    int64_t  started    = esp_timer_get_time();
    int64_t  win_start  = started;
    esp_err_t res       = ESP_OK;

    while (true)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL)
        {
            ESP_LOGW(TAG, "frame grab failed (psram exhausted or sensor stalled)");
            s_grab_fail++;
            res = ESP_FAIL;
            break;
        }
        if (fb->format != PIXFORMAT_JPEG)
        {
            ESP_LOGE(TAG, "unexpected non-JPEG frame format");
            esp_camera_fb_return(fb);
            res = ESP_FAIL;
            break;
        }
        s_width  = fb->width;
        s_height = fb->height;

        /* fb->len read before the return: the driver hands this descriptor to
         * the capture task the moment we give it back, so anything read after
         * it can be the next frame's metadata */
        size_t  fb_len    = fb->len;
        int64_t send_beg  = esp_timer_get_time();
        res = sink(req, fb, ++s_seq, ud);
        uint32_t send_ms  = (uint32_t)((esp_timer_get_time() - send_beg + 500) / 1000);
        esp_camera_fb_return(fb);

        if (send_ms > s_worst_send_ms)
        {
            s_worst_send_ms = send_ms;
        }
        if (send_ms >= STALL_SEND_MS)
        {
            /* throttled: a viewer that cannot drain its socket stalls every
             * frame, and one log line per stall would be the real stutter */
            if ((++s_stalls % 16u) == 1u)
            {
                ESP_LOGW(TAG, "send stalled %u times, last frame %u ms (worst %u ms)",
                         (unsigned)s_stalls, (unsigned)send_ms,
                         (unsigned)s_worst_send_ms);
            }
        }

        /* the client closed the page: the write is what notices */
        if (res != ESP_OK)
        {
            break;
        }

        frames++;
        win_frames++;
        s_frames++;
        bytes_sent += fb_len;

        int64_t now = esp_timer_get_time();
        if (now - win_start >= FPS_WINDOW_US)
        {
            s_fps_x10 = (uint32_t)((uint64_t)win_frames * 10000000ull /
                                   (uint64_t)(now - win_start));
            win_frames = 0;
            win_start  = now;
        }

        if (frames % 30 == 0)
        {
            ESP_LOGI(TAG, "%u frames, %.1f fps, %.1f KB/frame",
                     (unsigned)frames, frames * 1e6 / (double)(now - started),
                     bytes_sent / 1024.0 / (double)frames);
        }
    }

    s_clients--;
    s_fps_x10 = 0;      /* no viewer pulling: report 0 rather than a stale rate */
    ESP_LOGI(TAG, "%s closed after %u frames (err=%s)",
             what, (unsigned)frames, esp_err_to_name(res));
    return res;
}

static esp_err_t mjpeg_sink(httpd_req_t *req, const camera_fb_t *fb,
                            uint32_t seq, void *ud)
{
    char head[128];
    (void)seq;
    (void)ud;

    int head_len = snprintf(head, sizeof(head), STREAM_MJPEG_HEAD, (unsigned)fb->len);
    esp_err_t res = httpd_resp_send_chunk(req, head, head_len);
    res |= httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!viewer_attach(req, "mjpeg", STREAM_CONTENT_TYPE))
    {
        return ESP_FAIL;
    }
    return frame_pump(req, mjpeg_sink, NULL, "mjpeg");
}

/* ---- camera WebSocket: 8.2 text plane + gated binary plane -------------------
 * Session model (LLDD §8.2 / 表 15/17, doc/19 §5.2):
 *
 *   IDF 6.x esp_http_server completes the WS upgrade itself and never calls the
 *   URI handler for the GET (httpd_uri.c: "do not call the uri->handler"), so
 *   the session lifecycle hangs off the handshake callbacks:
 *
 *     pre-handshake  : claim the single viewer slot (refusing keeps HTTP mode)
 *     post-handshake : the 101 is on the wire - start the pump, send hello
 *     handler        : INBOUND plane only, re-entered once per text frame
 *
 *   Writes are the pump's (hello, binary frames, pong, 1 Hz cam_state) plus the
 *   rare handler reply (an err, a PONG for a PING), and every one of them takes
 *   s_ws.tx_mtx - see ws_send_frame().  The pump retires the slot on exit, so a
 *   handset that vanishes without a CLOSE frame cannot leave the only viewer
 *   position squatted.
 *
 *   Text ops are "op"-only JSON per contracts/vision/vision.h (CAM_WS_*), with
 *   the {"t":"cam_cmd",...} control-plane shape accepted for the same ops.
 *   Frames are grabbed even while unsubscribed (so re-subscribe is immediate)
 *   and counted into cam_state.drop instead of going over the air - the 表 15
 *   latest-only rule. */
#define WS_RX_TEXT_MAX    128u    /* inbound op frames above this are dropped */
#define WS_RX_CTRL_MAX    125u    /* RFC 6455: control-frame payload is <= 125 B */
#define WS_TX_LOCK_MS     600u    /* > the 500 ms SO_SNDTIMEO, so a lock timeout
                                   * can only mean a genuinely stalled peer      */
#define WS_TX_FAIL_MAX      3u    /* consecutive failed writes = the peer is gone */
#define WS_VGA_MIN_GAP_MS   400u   /* VGA pacing: ~2.5 fps = handset decode rate */
#define WS_FRAME_BUDGET_MS 1500u   /* whole-frame write budget (several SO_SNDTIMEO ticks) */
#define WS_STATE_PERIOD_MS 1000u
#define WS_TASK_STACK       3072

/* bits the pump task reacts to (handler = inbound + locked replies, pump = the
 * steady-state writer) */
#define PUMP_SUBSCRIBE  BIT(0)
#define PUMP_PAUSE      BIT(1)
#define PUMP_PROFILE    BIT(2)
#define PUMP_PING       BIT(3)
#define PUMP_CLOSE      BIT(4)
#define PUMP_HELLO      BIT(5)   /* pump has sent hello; the session is live   */
#define PUMP_WANTED     (PUMP_SUBSCRIBE | PUMP_PAUSE | PUMP_PROFILE | PUMP_PING)
#define PUMP_EXIT_BITS  (PUMP_CLOSE | PUMP_PROFILE | PUMP_PING)

typedef struct
{
    httpd_handle_t       hd;
    int                  fd;           /* -1 while the slot is free            */
    volatile bool        in_use;
    EventGroupHandle_t   bits;         /* lives as long as the server         */
    SemaphoreHandle_t    tx_mtx;       /* ditto; serialises this socket's TX  */
    volatile bool        subscribed;   /* set/cleared only via event bits    */
    volatile uint8_t     tx_fail;      /* consecutive failed writes          */
    char                 profile[16];
    uint32_t             started_ms;
    uint32_t             last_tx_ms;   /* last binary frame put on the air   */
} ws_session_t;

/* One handset at a time, so the session is a static: nothing to free under the
 * pump task, and no lifetime race between the handler call and the task. */
static ws_session_t s_ws = { .fd = -1 };

static const char *ws_profile_name(void)
{
    if (s_width == 320 && s_height == 240) {
        return CAM_PROFILE_REMOTE;
    }
    if (s_width == 640 && s_height == 480) {
        return CAM_PROFILE_WEB;
    }
    return "STREAM_PREVIEW";
}

/* Every transmit on this socket goes through here, for two reasons:
 *
 *   - Cross-task sends use the async API with an fd.  The httpd_req_t handed to
 *     a callback is per-request scratch the server reuses for the next session
 *     (one hd_req per instance), so a task that kept the pointer would end up
 *     writing JPEG bytes into whichever socket the httpd task happens to serve.
 *   - httpd_ws_send_frame_async() writes the WS header and the payload with two
 *     separate send() calls and takes no lock.  The pump and the handler (err /
 *     PONG replies) share this socket, and while handle_ws_control_frames was
 *     off the httpd task itself answered every PING - two writers interleaving
 *     byte-wise is what the peer then reads as a bogus opcode.  The control
 *     plane learned it first: s3_http/http_server.c:ws_tx. */
/* Write all of buf or report how far it got.  send() on this socket blocks for
 * at most SO_SNDTIMEO (500 ms) and lwIP then returns the bytes it DID queue -
 * a positive short count, not an error.  httpd_ws_send_frame_async() only
 * checks for < 0, so under Wi-Fi congestion a 20 KB JPEG went out truncated,
 * the next frame header landed mid-payload, and the handset's parser read JPEG
 * bytes as "RSV bits set" / "reserved opcode" and tore the camera WS down
 * (bench 10-02: every reconnect followed a "ws send stalled" line). */
static size_t ws_write_all(int fd, const uint8_t *buf, size_t len, int64_t deadline_us)
{
    size_t off = 0u;
    while (off < len)
    {
        int n = send(fd, buf + off, len - off, 0);
        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
            esp_timer_get_time() < deadline_us)
        {
            continue;                           /* SO_SNDTIMEO tick: keep going */
        }
        break;                                  /* hard error or out of budget */
    }
    return off;
}

/* Unmasked server->client frame, written to completion from up to two
 * contiguous parts (cam frame = 20 B header + the driver's fb, no staging
 * copy).  A frame that died after its first byte left can never be
 * resynchronised on the peer's side, so that case closes the socket instead
 * of leaving a torn stream behind - a clean redial is ~200 ms, a desynced
 * stream is a dead one. */
static esp_err_t ws_frame_write(ws_session_t *s, httpd_ws_type_t type,
                                size_t total_len,
                                const uint8_t *p1, size_t n1,
                                const uint8_t *p2, size_t n2)
{
    uint8_t hdr[10];
    size_t  hl = 0u;
    hdr[hl++] = (uint8_t)(0x80u | ((uint8_t)type & 0x0Fu));   /* FIN + opcode */
    if (total_len <= 125u)
    {
        hdr[hl++] = (uint8_t)total_len;
    }
    else if (total_len <= 0xFFFFu)
    {
        hdr[hl++] = 126u;
        hdr[hl++] = (uint8_t)(total_len >> 8);
        hdr[hl++] = (uint8_t)(total_len);
    }
    else
    {
        uint64_t l = total_len;
        hdr[hl++] = 127u;
        for (int i = 7; i >= 0; i--)
        {
            hdr[hl++] = (uint8_t)(l >> (i * 8));
        }
    }

    const int64_t deadline = esp_timer_get_time() + (int64_t)WS_FRAME_BUDGET_MS * 1000;
    size_t sent = ws_write_all(s->fd, hdr, hl, deadline);
    if (sent == hl && n1 > 0u && p1 != NULL)
    {
        sent += ws_write_all(s->fd, p1, n1, deadline);
        if (sent == hl + n1 && n2 > 0u && p2 != NULL)
        {
            sent += ws_write_all(s->fd, p2, n2, deadline);
        }
    }
    if (sent == hl + total_len)
    {
        return ESP_OK;
    }
    if (sent == 0u)
    {
        return ESP_ERR_TIMEOUT;                 /* nothing left: stream intact */
    }
    ESP_LOGW(TAG, "ws/camera torn write %u/%u B on fd=%d - closing to resync",
             (unsigned)sent, (unsigned)(hl + total_len), s->fd);
    s->tx_fail = WS_TX_FAIL_MAX;                /* pump retires on its next loop */
    (void)httpd_sess_trigger_close(s->hd, s->fd);
    return ESP_FAIL;
}

/* Locked sender, scatter form: type + total length with up to two payload
 * parts (part2 lets the binary path send straight out of the driver's fb).
 * Serialises against the handler's replies on this socket and keeps the
 * consecutive-failure count the pump retires on. */
static esp_err_t ws_send_frame_parts(ws_session_t *s, httpd_ws_type_t type,
                                     size_t total_len,
                                     const uint8_t *p1, size_t n1,
                                     const uint8_t *p2, size_t n2)
{
    if (xSemaphoreTake(s->tx_mtx, pdMS_TO_TICKS(WS_TX_LOCK_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ws_frame_write(s, type, total_len, p1, n1, p2, n2);
    (void)xSemaphoreGive(s->tx_mtx);
    if (err == ESP_FAIL)
    {
        return err;                             /* torn: tx_fail already maxed */
    }
    /* A peer that dies without a CLOSE frame shows up only as a failed write,
     * and while unsubscribed the cam_state tick is all this socket carries - so
     * the count, not the binary send, is what lets the pump notice. */
    s->tx_fail = (err == ESP_OK) ? 0u : (uint8_t)(s->tx_fail + 1u);
    return err;
}

static esp_err_t ws_send_frame(ws_session_t *s, httpd_ws_frame_t *pkt)
{
    return ws_send_frame_parts(s, pkt->type, pkt->len,
                               pkt->payload, pkt->len, NULL, 0u);
}

static void ws_send_text(ws_session_t *s, const char *json)
{
    httpd_ws_frame_t pkt = {
        .final   = true,
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };
    if (ws_send_frame(s, &pkt) != ESP_OK) {
        ESP_LOGW(TAG, "ws/camera text send failed (fd=%d)", s->fd);
    }
}

static void ws_send_hello(ws_session_t *s)
{
    /* 表 17: sensor/profile/fps/w/h (sd/rec dropped with the SD feature). */
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"hello\":true,\"t\":\"cam_hello\",\"sensor\":\"%s\","
             "\"profile\":\"%s\",\"w\":%u,\"h\":%u,\"ts\":%lu}",
             s_sensor, s->profile,
             (unsigned)s_width, (unsigned)s_height,
             (unsigned long)(esp_timer_get_time() / 1000));
    ws_send_text(s, buf);
    ESP_LOGI(TAG, "ws/camera hello sent (%s %s)", s_sensor, s->profile);
}

static void ws_send_pong(ws_session_t *s)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"op\":\"" CAM_WS_OP_PONG_NAME "\",\"ts\":%lu}",
             (unsigned long)(esp_timer_get_time() / 1000));
    ws_send_text(s, buf);
}

static void ws_send_state(ws_session_t *s)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"cam_state\",\"sensor\":\"%s\",\"profile\":\"%s\","
             "\"w\":%u,\"h\":%u,\"fps\":%u.%u,\"seq\":%lu,\"drop\":%lu,"
             "\"subscribed\":%s,\"ts\":%lu}",
             s_sensor, s->profile,
             (unsigned)s_width, (unsigned)s_height,
             (unsigned)(s_fps_x10 / 10u), (unsigned)(s_fps_x10 % 10u),
             (unsigned long)s_seq, (unsigned long)s_ws_dropped,
             s->subscribed ? "true" : "false",
             (unsigned long)(esp_timer_get_time() / 1000));
    ws_send_text(s, buf);
}

/* 表 8 preview profiles are the two sizes the OV5640 does well; anything else
 * in the name is rejected with err{e:"bad"}.  set_framesize on a JPEG sensor
 * only rewrites registers - the next fb carries the new dims. */
static bool ws_apply_profile(ws_session_t *s, const char *name)
{
    int size;
    if (strcmp(name, CAM_PROFILE_REMOTE) == 0) {
        size = FRAMESIZE_QVGA;   /* 320x240 */
    } else if (strcmp(name, CAM_PROFILE_WEB) == 0) {
        size = FRAMESIZE_VGA;    /* 640x480 */
    } else {
        return false;
    }

    sensor_t *sen = esp_camera_sensor_get();
    if (sen == NULL || sen->set_framesize(sen, (framesize_t)size) != 0) {
        ESP_LOGW(TAG, "ws/camera set_framesize(%s) failed", name);
        return false;
    }
    strlcpy(s->profile, name, sizeof(s->profile));
    {
        uint16_t w = 0, h = 0;
        frame_size_dims((framesize_t)size, &w, &h);
        s_width  = w;
        s_height = h;
    }
    ESP_LOGI(TAG, "ws/camera profile -> %s", name);
    return true;
}

static void ws_pump_task(void *arg)
{
    ws_session_t *s = (ws_session_t *)arg;

    /* The handset's state machine keys off hello (SUBSCRIBING is only entered
     * once it lands), so it goes out before anything else on this socket. */
    ws_send_hello(s);
    xEventGroupSetBits(s->bits, PUMP_HELLO);
    uint32_t next_state = 1u;          /* first cam_state after ~1 s */
    EventBits_t bits;

    while (true)
    {
        if (s->tx_fail >= WS_TX_FAIL_MAX)
        {
            ESP_LOGW(TAG, "ws/camera: %u writes in a row failed, retiring session",
                     (unsigned)s->tx_fail);
            break;
        }
        bits = xEventGroupWaitBits(s->bits, PUMP_EXIT_BITS, pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(50));
        if (bits & PUMP_PAUSE) {
            s->subscribed = false;
        }
        if (bits & PUMP_SUBSCRIBE) {
            s->subscribed = true;
        }
        if (bits & PUMP_PROFILE) {
            ws_send_state(s);
        }
        if (bits & PUMP_PING) {
            ws_send_pong(s);
        }
        if (bits & PUMP_CLOSE) {
            break;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL)
        {
            s_grab_fail++;
            if (esp_timer_get_time() / 1000 - (int64_t)s->started_ms > 5000) {
                ESP_LOGE(TAG, "ws/camera: sensor silent 5 s, dropping session");
                break;
            }
            continue;
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms >= next_state) {
            next_state = now_ms + WS_STATE_PERIOD_MS;
            ws_send_state(s);
        }

        bool send = s->subscribed && fb->format == PIXFORMAT_JPEG;
        /* Pace VGA to what the handset can show: it decodes a 640x480 frame in
         * ~450 ms, so at the sensor's ~11 fps three of every four VGA frames
         * crossed the air only to be dropped latest-only on arrival (bench
         * 10-02: drop=742 of ~1000).  That wasted airtime is what starved the
         * handset's control uplink into RADIO LOST.  QVGA (~70 ms decode)
         * keeps the full rate. */
        if (send && fb->width > 320u) {
            if (now_ms - s->last_tx_ms < WS_VGA_MIN_GAP_MS) {
                send = false;
                s_ws_dropped++;
            }
        }

        if (send)
        {
            /* Zero-copy TX: the fb stays owned until after the send, so the
             * JPEG goes straight out of the driver's PSRAM buffer - only the
             * 20 B cam header is composed locally.  (The old obuf staging
             * cost one full-frame memcpy per frame plus its grow/free
             * machinery, and bought nothing: the send already sat between
             * fb_get and fb_return.) */
            uint8_t cam_hdr[CAM_HEADER_LEN];
            /* ++: SEQ must advance per SENT frame (表 14 monotonic).  The
             * /stream path bumps it via its sink argument; the WS path sends
             * straight from s_seq, so a constant 0 landed in every header and
             * the handset's cam_seq_note binned all frames after the first as
             * CAM_SEQ_STALE duplicates - the video plane showed one frozen
             * frame per connection, i.e. never a live picture. */
            size_t hl = cam_frame_build(++s_seq,
                                        (uint32_t)fb->timestamp.tv_sec * 1000u +
                                        (uint32_t)(fb->timestamp.tv_usec / 1000),
                                        fb->width, fb->height, (uint32_t)fb->len,
                                        CAM_FLAG_KEYFRAME,
                                        cam_hdr, sizeof(cam_hdr));
            if (hl == 0u) {
                s_ws_dropped++;      /* oversized/corrupt fb: latest-only drop */
            } else {
                int64_t send_beg = esp_timer_get_time();
                esp_err_t res = ws_send_frame_parts(s, HTTPD_WS_TYPE_BINARY,
                                                    hl + fb->len,
                                                    cam_hdr, hl,
                                                    fb->buf, fb->len);
                uint32_t send_ms = (uint32_t)((esp_timer_get_time() - send_beg + 500) / 1000);
                if (send_ms > s_worst_send_ms) {
                    s_worst_send_ms = send_ms;
                }
                if (send_ms >= STALL_SEND_MS && ((++s_stalls % 16u) == 1u)) {
                    ESP_LOGW(TAG, "ws send stalled %u times, last frame %u ms (worst %u ms)",
                             (unsigned)s_stalls, (unsigned)send_ms,
                             (unsigned)s_worst_send_ms);
                }
                if (res == ESP_ERR_TIMEOUT) {
                    /* not one byte left: the stream is still in sync, so this
                     * is a latest-only drop; WS_TX_FAIL_MAX in a row retires */
                    s_ws_dropped++;
                } else if (res != ESP_OK) {
                    esp_camera_fb_return(fb);
                    ESP_LOGW(TAG, "ws/camera send failed: %s", esp_err_to_name(res));
                    break;
                } else {
                    s_frames++;
                    s->last_tx_ms = now_ms;
                }
            }
        }
        else if (!s->subscribed)
        {
            s_ws_dropped++;          /* fresh-but-undelivered (preview paused) */
        }

        esp_camera_fb_return(fb);
    }

    s_fps_x10 = 0;
    /* The pump retires the slot: a handset that dies without a CLOSE frame
     * must not keep the only viewer position, or the next dial would be refused
     * forever.  Bits are cleared before in_use drops, so the next session starts
     * from a clean mailbox. */
    s->subscribed = false;
    xEventGroupClearBits(s->bits, PUMP_WANTED | PUMP_CLOSE | PUMP_HELLO);
    s->fd     = -1;
    s->in_use = false;
    /* framesize is one set of registers shared with /stream, and the handset asked
     * for its own size on the text plane: without this the phone page would keep
     * the handset's 320x240 long after the handset left.  Restore the Kconfig size
     * the device was built for (register-only write, the driver is not restarted). */
    {
        sensor_t *sen = esp_camera_sensor_get();
        if (sen != NULL && sen->set_framesize(sen, CAM_FRAME_SIZE) == 0) {
            uint16_t w = 0, h = 0;
            frame_size_dims(CAM_FRAME_SIZE, &w, &h);
            s_width  = w;
            s_height = h;
        }
    }
    vTaskDelete(NULL);
}

/* inbound text plane: {"op":...} per LLDD §8.2 / 表 17; the control-plane
 * shape {"t":"cam_cmd","op":...,"name":...} carries the same ops and is
 * accepted here too (the handset currently sends op-only on this plane). */
static void ws_handle_ops(ws_session_t *s, const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL)
    {
        ESP_LOGW(TAG, "ws/camera: malformed text op dropped");
        return;
    }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    if (!cJSON_IsString(op))
    {
        cJSON_Delete(root);            /* no op: not ours (t-only JSON etc.) */
        return;
    }
    const char *name = op->valuestring;
    ESP_LOGI(TAG, "ws/camera op: %s (fd=%d)", name, s->fd);

    if (strcmp(name, CAM_WS_OP_SUBSCRIBE) == 0)
    {
        xEventGroupSetBits(s->bits, PUMP_SUBSCRIBE);
    }
    else if (strcmp(name, CAM_WS_OP_PAUSE) == 0)
    {
        xEventGroupSetBits(s->bits, PUMP_PAUSE);
    }
    else if (strcmp(name, CAM_WS_OP_PING) == 0)
    {
        xEventGroupSetBits(s->bits, PUMP_PING);
    }
    else if (strcmp(name, CAM_OP_START) == 0)
    {
        xEventGroupSetBits(s->bits, PUMP_SUBSCRIBE);
    }
    else if (strcmp(name, CAM_OP_STOP) == 0)
    {
        xEventGroupSetBits(s->bits, PUMP_PAUSE);
    }
    else if (strcmp(name, CAM_OP_PROFILE) == 0)
    {
        const cJSON *prof = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (!cJSON_IsString(prof) || !ws_apply_profile(s, prof->valuestring))
        {
            ws_send_text(s, "{\"t\":\"" VISION_T_ERR "\",\"e\":\"bad\"}");
        }
        else
        {
            xEventGroupSetBits(s->bits, PUMP_PROFILE);
        }
    }
    else
    {
        ESP_LOGW(TAG, "ws/camera: unknown op \"%s\"", name);
        ws_send_text(s, "{\"t\":\"" VISION_T_ERR "\",\"e\":\"bad\"}");
    }
    cJSON_Delete(root);
}

/* pre-handshake: claim the single viewer slot.  Nothing may be written to the
 * socket here - bytes ahead of the 101 interleave into the handshake and the
 * peer aborts the upgrade (learned the hard way on the control plane, s3_http). */
static esp_err_t ws_camera_pre_handshake(httpd_req_t *req)
{
    if (!s_ready)
    {
        return ESP_FAIL;                    /* no sensor: stay in HTTP mode */
    }
    if (s_ws.in_use)
    {
        /* A handset can vanish without a CLOSE frame (its socket dies mid send).
         * Only an fd the server no longer tracks as a live WS session is stale,
         * so a dropped handset never locks the camera out of the next dial. */
        if (httpd_ws_get_fd_info(s_ws.hd, s_ws.fd) == HTTPD_WS_CLIENT_WEBSOCKET)
        {
            ESP_LOGW(TAG, "ws/camera busy on fd=%d - one viewer at a time", s_ws.fd);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "ws/camera reclaiming dead fd=%d", s_ws.fd);
        xEventGroupSetBits(s_ws.bits, PUMP_CLOSE);
        for (int i = 0; i < 60 && s_ws.in_use; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));  /* the pump retires its own slot */
        }
        if (s_ws.in_use) {
            return ESP_FAIL;
        }
    }

    s_ws.fd          = httpd_req_to_sockfd(req);
    s_ws.subscribed  = false;  /* gated: nothing on the air until subscribe */
    s_ws.tx_fail     = 0u;     /* the slot is static: stale counts would retire the new pump */
    s_ws.started_ms  = (uint32_t)(esp_timer_get_time() / 1000);
    strlcpy(s_ws.profile, ws_profile_name(), sizeof(s_ws.profile));
    s_ws.in_use      = true;
    return ESP_OK;
}

/* post-handshake: the 101 is on the wire, so WS frames are legal from here on.
 * The pump sends hello itself, which also starts the binary gate. */
static esp_err_t ws_camera_post_handshake(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    const struct timeval snd = { .tv_sec = 0, .tv_usec = 500u * 1000u };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    const int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (xTaskCreatePinnedToCore(ws_pump_task, "cam_ws_tx", WS_TASK_STACK,
                               &s_ws, 4, NULL, 0) != pdPASS)
    {
        s_ws.in_use = false;
        s_ws.fd     = -1;
        ESP_LOGE(TAG, "ws/camera pump task could not start");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ws/camera session on fd=%d (%s)", fd, s_ws.profile);
    return ESP_OK;
}

/* INBOUND plane only (IDF 6.x never calls the handler for the upgrade GET).
 * The URI also takes handle_ws_control_frames, so PING/CLOSE land here rather
 * than being answered by the httpd task - its auto-PONG would be a second,
 * unserialised writer on a socket the pump is mid-frame on. */
static esp_err_t ws_camera_handler(httpd_req_t *req)
{
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    if (httpd_ws_recv_frame(req, &pkt, 0) != ESP_OK)
    {
        return ESP_FAIL;                    /* socket error: nothing left to serve */
    }
    if (!s_ws.in_use || s_ws.fd != httpd_req_to_sockfd(req))
    {
        /* Not our session (already retired, or a second viewer snuck in).
         * Hanging up is the only safe answer: returning ESP_OK would leave the
         * body queued, and the next header parse would read payload bytes as an
         * opcode - that is what logs "WS frame is not properly masked". */
        return ESP_FAIL;
    }

    if (pkt.type == HTTPD_WS_TYPE_CLOSE)
    {
        ESP_LOGI(TAG, "ws/camera hangup on fd=%d", s_ws.fd);
        xEventGroupSetBits(s_ws.bits, PUMP_CLOSE);
        return ESP_FAIL;     /* the pump frees the slot when it wakes up */
    }
    if ((pkt.type == HTTPD_WS_TYPE_PING) || (pkt.type == HTTPD_WS_TYPE_PONG))
    {
        /* RFC 6455 5.5.3: the PONG echoes the PING payload.  A PONG can only be
         * an answer to a frame we never sent, so it is dropped - but its body
         * has to leave the socket either way, or it parses as a frame header. */
        uint8_t  body[WS_RX_CTRL_MAX];
        size_t   n = (pkt.len > sizeof(body)) ? sizeof(body) : pkt.len;

        if (n > 0u)
        {
            pkt.payload = body;
            if (httpd_ws_recv_frame(req, &pkt, n) != ESP_OK)
            {
                return ESP_FAIL;
            }
        }
        if (pkt.type == HTTPD_WS_TYPE_PING)
        {
            httpd_ws_frame_t pong = {
                .final   = true,
                .type    = HTTPD_WS_TYPE_PONG,
                .payload = body,
                .len     = n,
            };
            if (ws_send_frame(&s_ws, &pong) != ESP_OK) {
                ESP_LOGW(TAG, "ws/camera pong failed on fd=%d", s_ws.fd);
            }
        }
        return ESP_OK;
    }
    if (pkt.len == 0u)
    {
        return ESP_OK;                      /* empty data frame: nothing to consume */
    }
    if (pkt.len > WS_RX_TEXT_MAX || pkt.type != HTTPD_WS_TYPE_TEXT)
    {
        return ESP_FAIL;                    /* protocol violation; body dies with the socket */
    }

    char text[WS_RX_TEXT_MAX + 1];
    pkt.payload = (uint8_t *)text;
    if (httpd_ws_recv_frame(req, &pkt, pkt.len) != ESP_OK)
    {
        return ESP_FAIL;
    }
    text[pkt.len] = '\0';
    (void)ws_handle_ops(&s_ws, text);
    return ESP_OK;                          /* next inbound frame re-enters here */
}

esp_err_t camera_start(void)
{
    return camera_init();
}

esp_err_t camera_stream_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = CONFIG_S3_CAMERA_STREAM_PORT;
    config.ctrl_port        = CONFIG_S3_CAMERA_STREAM_PORT + 1000;
    config.max_uri_handlers = 2;      /* /stream + /ws/camera */
    /* below the control-plane httpd (default tskIDLE_PRIORITY+5): a viewer
     * saturating its socket must never delay commands or telemetry */
    config.task_priority    = tskIDLE_PRIORITY + 3;
    /* doc/20 核分工: capture runs on core 1, every socket write on core 0 */
    config.core_id          = 0;
    /* a viewer that stops draining its socket - e.g. a handset that died mid
     * frame - must not pin the single stream task forever (default 5 s).
     * 2 s is well above any honest stall (a VGA frame at the tuned TX window
     * leaves the socket in tens of ms) yet bounds how long one dead client can
     * deny the picture to the next one - the page retries after onerror. */
    config.send_wait_timeout = 2;
    /* Bounds one blocking read inside a handler turn.  Note it does NOT close an
     * idle socket: IDF's httpd_server() selects without a timeout, so a handset
     * that goes silent is retired by the pump's next failed write (500 ms
     * SO_SNDTIMEO) or reclaimed by the next dial, not by this counter. */
    config.recv_wait_timeout = 5;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "stream httpd start failed: %s", esp_err_to_name(err));
        return err;
    }
    /* The WS mailbox and TX lock live as long as the server: sessions come and
     * go, but the pump task must never be handed an object someone else deletes. */
    if (s_ws.bits == NULL)
    {
        s_ws.bits = xEventGroupCreate();
        if (s_ws.bits == NULL)
        {
            (void)httpd_stop(server);
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_ws.tx_mtx == NULL)
    {
        s_ws.tx_mtx = xSemaphoreCreateMutex();
        if (s_ws.tx_mtx == NULL)
        {
            (void)httpd_stop(server);
            return ESP_ERR_NO_MEM;
        }
    }
    s_ws.hd = server;

    const httpd_uri_t stream_uri = {
        .uri      = "/stream",
        .method   = HTTP_GET,
        .handler  = stream_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(server, &stream_uri);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "stream uri register failed: %s", esp_err_to_name(err));
    }

    const httpd_uri_t ws_uri = {
        .uri                      = "/ws/camera",
        .method                   = HTTP_GET,
        .handler                  = ws_camera_handler,
        .user_ctx                 = NULL,
        .is_websocket             = true,
        /* take the control frames off httpd's hands: the handset's
         * esp_websocket_client pings every 10 s and drops a link that answers
         * nothing, and the stack's answer would race the pump's writes */
        .handle_ws_control_frames = true,
        .ws_pre_handshake_cb      = ws_camera_pre_handshake,
        .ws_post_handshake_cb     = ws_camera_post_handshake,
    };
    err |= httpd_register_uri_handler(server, &ws_uri);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "camera ws uri register failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "MJPEG stream on http://<ap>:%d/stream, camera ws on ws://<ap>:%d/ws/camera",
             CONFIG_S3_CAMERA_STREAM_PORT, CONFIG_S3_CAMERA_STREAM_PORT);
    return err;
}

bool camera_available(void)
{
    return s_ready;
}

const char *camera_sensor_name(void)
{
    return s_ready ? s_sensor : NULL;
}

void camera_stats(camera_stats_t *out)
{
    if (out == NULL)
    {
        return;
    }
    *out = (camera_stats_t){
        .width        = s_ready ? s_width  : 0u,
        .height       = s_ready ? s_height : 0u,
        .fps_x10      = s_fps_x10,
        .frames       = s_frames,
        .grab_fail    = s_grab_fail,
        .stalls       = s_stalls,
        .worst_send_ms = s_worst_send_ms,
        .drop         = s_ws_dropped,
        .viewers      = (uint8_t)s_clients,
    };
}
