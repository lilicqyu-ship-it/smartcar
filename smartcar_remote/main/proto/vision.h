/*
 * vision.h - SmartCar Vision / Camera text-plane message schema (single source of truth)
 *
 * JSON contract for the Control WebSocket text plane (ws://<gateway>/ws), per
 * S3CAM LLDD V1.0 §11 (vision results) and §12 (camera control), Remote design
 * doc smartcar_remote/doc/SmartCar_S3Remote_详细设计说明书_V1.0.md §7.2.
 *
 * Both sides share this file:
 *   - smartcar_remote (S3) : scr_link RX dispatch + command builders (main/proto/)
 *   - s3-gateway  (S3-CAM) : TX producers + command parsers (to be adopted with
 *                            the camera_ws / vision components, design G-4..G-6)
 *
 * Hard rules (same conventions as contracts/link/proto_frames.h):
 *   - Pure C99, no OS/IDF headers, host-compilable, header-only.
 *   - This file fixes NAMES and value sets only; encode/decode stays with
 *     cJSON on both ends (messages are small; no binary layout here - that is
 *     cam_frame.h's job).
 *
 * Wire examples:
 *   gateway -> remote : {"t":"vision","ver":1,"mode":"line","valid":true,
 *                        "confidence":0.92,"cx":163,"error":-0.018,
 *                        "angle":-4.2,"ts":123456}
 *   gateway -> remote : {"t":"vision","ver":1,"mode":"object","count":1,
 *                        "objects":[{"class":1,"score":0.91,"x":55,"y":30,"w":80,"h":120}]}
 *   remote -> gateway : {"t":"vision_cmd","mode":"line","enable":true}      (CTRL)
 *   remote -> gateway : {"t":"drive_mode","mode":"assist"}                  (CTRL)
 *   gateway -> remote : {"t":"drive_mode","mode":"assist","ok":true}        (ack)
 *
 * Storage ops are NOT part of this contract: the S3-CAM has no MicroSD,
 * snapshot/record were dropped from the V1.0 feature set.
 *
 * Units (frozen here, LLDD §11 + Remote design §11 risk 3):
 *   confidence : float 0..1  (remote stores percent 0..100)
 *   error      : float -1..1 normalized line-center offset (remote x1000 i16)
 *   angle      : float degrees (remote x10 i16)
 *   cx,x,y,w,h : integer pixels in the CURRENT PREVIEW frame coordinate space
 *                (profile w/h from cam_hello/cam_state, NOT the vision-only
 *                resolution; gateway rescales if the two differ)
 *   ts         : u32 ms, gateway uptime of the producing frame
 */
#ifndef VISION_H
#define VISION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- message types (value of the "t" key) ----------------------------------*/
#define VISION_T_VISION        "vision"       /* gw -> remote, result snapshot */
#define VISION_T_VISION_CMD    "vision_cmd"   /* remote -> gw, CTRL           */
#define VISION_T_DRIVE_MODE    "drive_mode"   /* both ways (up = command)     */
#define VISION_T_CAM_CMD       "cam_cmd"      /* remote -> gw, CTRL          */
#define VISION_T_CAM_STATE     "cam_state"    /* gw -> remote, Camera WS text */
#define VISION_T_CAM_HELLO     "cam_hello"    /* gw -> remote, Camera WS text */
#define VISION_T_ERR           "err"          /* gw -> remote/ctrl, {"e":...} */

#define VISION_JSON_VER        1              /* "ver" field of vision msgs   */
#define VISION_CMD_BUF_MAX     96             /* builder output cap           */

/* ---- JSON field names ------------------------------------------------------*/
#define VISION_F_T             "t"
#define VISION_F_VER           "ver"
#define VISION_F_MODE          "mode"
#define VISION_F_ENABLE        "enable"
#define VISION_F_VALID         "valid"
#define VISION_F_CONFIDENCE    "confidence"
#define VISION_F_CX            "cx"
#define VISION_F_ERROR         "error"
#define VISION_F_ANGLE         "angle"
#define VISION_F_TS            "ts"
#define VISION_F_COUNT         "count"
#define VISION_F_OBJECTS       "objects"
#define VISION_F_OK            "ok"

/* ---- vision modes -----------------------------------------------------------*/
#define VISION_MODE_OFF        "off"
#define VISION_MODE_LINE       "line"
#define VISION_MODE_COLOR      "color"
#define VISION_MODE_QR         "qr"
#define VISION_MODE_OBJECT     "object"

/* ---- drive assist modes (LLDD §10.2: AUTO is not implemented in V1.0; the
 * remote shows the button disabled, the gateway rejects the command) ---------*/
#define VISION_DRIVE_MANUAL    "manual"
#define VISION_DRIVE_ASSIST    "assist"
#define VISION_DRIVE_AUTO      "auto"

/* ---- camera control ops ("op" of cam_cmd) -----------------------------------*/
#define CAM_OP_PROFILE         "profile"     /* {"op":"profile","name":...}    */
#define CAM_OP_START           "start"       /* preview on  (Camera WS plane)  */
#define CAM_OP_STOP            "stop"        /* preview off (Camera WS plane)  */

/* ---- camera WS text-plane ops ("op" only, no "t"; LLDD §8.2) --------------*/
#define CAM_WS_SUBSCRIBE       "{\"op\":\"subscribe\"}"
#define CAM_WS_PAUSE           "{\"op\":\"pause\"}"
#define CAM_WS_PING            "{\"op\":\"ping\"}"
#define CAM_WS_PONG            "{\"op\":\"pong\"}"

/* op-name values for parsers (the bare "op" strings inside the JSONs above) */
#define CAM_WS_OP_SUBSCRIBE    "subscribe"
#define CAM_WS_OP_PAUSE        "pause"
#define CAM_WS_OP_PING         "ping"
#define CAM_WS_OP_PONG_NAME    "pong"

/* ---- preview profile names (LLDD Table 8) -----------------------------------*/
#define CAM_PROFILE_REMOTE     "REMOTE_PREVIEW"   /* 320x240 JPEG 8-10 fps  */
#define CAM_PROFILE_WEB        "WEB_PREVIEW"      /* 640x480 JPEG 5-10 fps  */

/* ---- builders (fixed strings; snprintf keeps both ends byte-identical) ------*/

/* {"t":"vision_cmd","mode":"line","enable":true} */
static inline size_t vision_fmt_mode_cmd(char *buf, size_t cap,
                                         const char *mode, bool enable)
{
    int n = snprintf(buf, cap, "{\"t\":\"" VISION_T_VISION_CMD "\",\"mode\":\"%s\",\"enable\":%s}",
                     mode, enable ? "true" : "false");
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* {"t":"drive_mode","mode":"assist"} */
static inline size_t vision_fmt_drive_mode(char *buf, size_t cap, const char *mode)
{
    int n = snprintf(buf, cap, "{\"t\":\"" VISION_T_DRIVE_MODE "\",\"mode\":\"%s\"}", mode);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* {"t":"cam_cmd","op":"profile","name":"REMOTE_PREVIEW"} */
static inline size_t vision_fmt_cam_profile(char *buf, size_t cap, const char *name)
{
    int n = snprintf(buf, cap,
                     "{\"t\":\"" VISION_T_CAM_CMD "\",\"op\":\"" CAM_OP_PROFILE "\",\"name\":\"%s\"}", name);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* mode string -> small enum index for state storage; -1 = unknown */
enum {
    VISION_MODE_IDX_OFF = 0,
    VISION_MODE_IDX_LINE,
    VISION_MODE_IDX_COLOR,
    VISION_MODE_IDX_QR,
    VISION_MODE_IDX_OBJECT,
    VISION_MODE_IDX_COUNT
};

static inline int vision_mode_from_str(const char *s)
{
    if (s == NULL) {
        return -1;
    }
    if (strcmp(s, VISION_MODE_OFF) == 0)    { return VISION_MODE_IDX_OFF; }
    if (strcmp(s, VISION_MODE_LINE) == 0)   { return VISION_MODE_IDX_LINE; }
    if (strcmp(s, VISION_MODE_COLOR) == 0)  { return VISION_MODE_IDX_COLOR; }
    if (strcmp(s, VISION_MODE_QR) == 0)     { return VISION_MODE_IDX_QR; }
    if (strcmp(s, VISION_MODE_OBJECT) == 0) { return VISION_MODE_IDX_OBJECT; }
    return -1;
}

static inline const char *vision_mode_str(int idx)
{
    switch (idx) {
        case VISION_MODE_IDX_OFF:    return VISION_MODE_OFF;
        case VISION_MODE_IDX_LINE:   return VISION_MODE_LINE;
        case VISION_MODE_IDX_COLOR:  return VISION_MODE_COLOR;
        case VISION_MODE_IDX_QR:     return VISION_MODE_QR;
        case VISION_MODE_IDX_OBJECT: return VISION_MODE_OBJECT;
        default:                     return "?";
    }
}

static inline int vision_drive_mode_from_str(const char *s)
{
    if (s == NULL) {
        return -1;
    }
    if (strcmp(s, VISION_DRIVE_MANUAL) == 0) { return 0; }
    if (strcmp(s, VISION_DRIVE_ASSIST) == 0) { return 1; }
    if (strcmp(s, VISION_DRIVE_AUTO) == 0)   { return 2; }
    return -1;
}

static inline const char *vision_drive_mode_str(int idx)
{
    switch (idx) {
        case 0:  return VISION_DRIVE_MANUAL;
        case 1:  return VISION_DRIVE_ASSIST;
        case 2:  return VISION_DRIVE_AUTO;
        default: return "?";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* VISION_H */
