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

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* S3_CAMERA_STREAM_H */
