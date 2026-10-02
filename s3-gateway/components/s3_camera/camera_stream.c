/*
 * camera_stream.c - OV5640 capture + MJPEG stream server (s3_camera)
 *
 * Ported from the bring-up bench app: the Freenove ESP32-S3-WROOM CAM is
 * pin-compatible with CAMERA_MODEL_ESP32S3_EYE, JPEG frames straight out of
 * the sensor (no encode path on the S3), delivered as multipart/x-mixed-replace.
 */
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_stream.h"

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
static const char *STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static volatile bool         s_ready;
static volatile unsigned     s_clients;
static char                  s_sensor[16];

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
        .frame_size   = (framesize_t)CONFIG_S3_CAMERA_FRAME_SIZE,
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
    ESP_LOGI(TAG, "sensor %s up: frame_size=%d quality=%d",
             s_sensor, CONFIG_S3_CAMERA_FRAME_SIZE, CONFIG_S3_CAMERA_JPEG_QUALITY);
    s_ready = true;
    return ESP_OK;
}

/* One viewer at a time: two tasks pulling from the same 2-frame queue each
 * drop half the frames and the second one adds latency to the first. */
static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!s_ready)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera unavailable");
        return ESP_FAIL;
    }
    if (s_clients >= 1u)
    {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "stream busy - one viewer at a time");
    }
    s_clients++;

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char    part_buf[64];
    uint32_t frames     = 0;
    size_t   bytes_sent = 0;
    int64_t  started    = esp_timer_get_time();
    esp_err_t res       = ESP_OK;

    while (true)
    {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL)
        {
            ESP_LOGW(TAG, "frame grab failed (psram exhausted or sensor stalled)");
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

        int part_len = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned)fb->len);
        res |= httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        res |= httpd_resp_send_chunk(req, part_buf, part_len);
        res |= httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        bytes_sent += fb->len;
        esp_camera_fb_return(fb);

        /* the client closed the page: the write is what notices */
        if (res != ESP_OK)
        {
            break;
        }

        if (++frames % 30 == 0)
        {
            int64_t elapsed = esp_timer_get_time() - started;
            ESP_LOGI(TAG, "%u frames, %.1f fps, %.1f KB/frame",
                     (unsigned)frames, frames * 1e6 / (double)elapsed,
                     bytes_sent / 1024.0 / (double)frames);
        }
    }

    s_clients--;
    ESP_LOGI(TAG, "stream closed after %u frames", (unsigned)frames);
    return res;
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
    config.max_uri_handlers = 2;
    /* below the control-plane httpd (default tskIDLE_PRIORITY+5): a viewer
     * saturating its socket must never delay commands or telemetry */
    config.task_priority    = tskIDLE_PRIORITY + 3;
    /* doc/20 核分工: capture runs on core 1, every socket write on core 0 */
    config.core_id          = 0;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "stream httpd start failed: %s", esp_err_to_name(err));
        return err;
    }

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
    ESP_LOGI(TAG, "MJPEG stream on http://<ap>:%d/stream", CONFIG_S3_CAMERA_STREAM_PORT);
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
