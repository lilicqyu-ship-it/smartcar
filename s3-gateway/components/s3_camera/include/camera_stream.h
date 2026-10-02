/*
 * camera_stream.h - OV5640 capture + MJPEG stream server (s3_camera)
 *
 * The stream runs on its OWN esp_http_server instance: a blocking MJPEG
 * handler would starve the single httpd task that also serves the control
 * page and every WebSocket frame, so video never shares a task with control.
 */
#ifndef S3_CAMERA_STREAM_H
#define S3_CAMERA_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Live view for /api/diag (详细设计说明书 18.2 camera{}).  fps is a rolling
 * window and reads 0 while no viewer is attached; frames/grab_fail are
 * cumulative since boot.  fps is scaled by 10 so the field is one atomic
 * word - the producer runs on core 1 while the reader is an httpd task.
 * stalls counts frames whose socket write alone took STALL_SEND_MS or longer
 * (the viewer, not the sensor, is the bottleneck); worst_send_ms is the
 * slowest such write since boot - together they turn "看着卡" into a number.
 * drop counts /ws/camera frames grabbed but not delivered (unsubscribed pause
 * window or oversized/corrupt fb), the same number cam_state carries. */
typedef struct
{
    uint16_t width;
    uint16_t height;
    uint32_t fps_x10;
    uint32_t frames;
    uint32_t grab_fail;
    uint32_t stalls;
    uint32_t worst_send_ms;
    uint32_t drop;
    uint8_t  viewers;
} camera_stats_t;

/* Probe and configure the sensor.  Call this before the network comes up:
 * XCLK and the sensor itself draw current, and a softAP already running on
 * 20 dBm makes a brownout ambiguous.  A missing or broken sensor is not
 * fatal - it only logs (plus an SCCB bus scan as evidence), and the stream
 * handler answers 503 for the whole life of the process. */
esp_err_t camera_start(void);

/* Publish the MJPEG endpoint.  Must run AFTER net_start(): esp_http_server
 * needs lwIP, and calling it earlier trips the tcpip thread assert. */
esp_err_t camera_stream_start(void);

/* true once the sensor has been detected and configured */
bool camera_available(void);

/* human-readable sensor name ("OV5640"), or NULL when absent */
const char *camera_sensor_name(void);

/* fill *out with the live stream counters; zeroed when the sensor is absent */
void camera_stats(camera_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* S3_CAMERA_STREAM_H */
