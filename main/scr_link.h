/*
 * scr_link.h - S3 <-> C6 link: Wi-Fi STA + WebSocket + proto v2 (spec 95-97)
 *
 * The remote is a Wi-Fi station on the C6 softAP and speaks exactly the same
 * WebSocket protocol as the phone control page of c6_car (doc 02/05/06/07):
 *   - binary frames: proto v2, DRIVE 0x50 at 30 Hz doubles as heartbeat
 *   - telemetry 0x41 broadcast to every session
 *   - text control plane JSON: hello / tc / pong / err
 *   - pairing through POST /api/pair (car-side window), token -> NVS
 *
 * The C6 firmware is NOT modified in any way (spec 96/97).
 */
#ifndef SCR_LINK_H
#define SCR_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start Wi-Fi + WebSocket + monitor task.  Non-blocking. */
void scr_link_start(void);

/* Thread-safe binary send (proto frame).  true if handed to the socket. */
bool scr_link_send_bin(const uint8_t *data, size_t len);

/* Thread-safe text send (JSON control plane, e.g. {"t":"ping"}). */
bool scr_link_send_text(const char *text);

/* Monotonic per-session frame sequence for proto SEQ. */
uint8_t scr_link_next_seq(void);

/* Request pairing (runs in the link monitor task).  Feedback lands in
 * app_state pair_status. */
void scr_link_request_pair(void);

/* Re-apply Wi-Fi credentials from settings and reconnect (Radio settings). */
void scr_link_apply_wifi(void);

#ifdef __cplusplus
}
#endif

#endif /* SCR_LINK_H */
