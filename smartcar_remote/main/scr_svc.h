/*
 * scr_svc.h - core-0 service task (doc/08-architecture-v2.md §2, §4-§6)
 *
 * Everything that may block on the network or on flash lives here, never on
 * core 1 (ctrl + LVGL):
 *   - firmware staging areas fw_c6 / fw_tc (SCFW header) and the streamed
 *     HTTP OTA upload to the C6 (/ota/c6, /ota/tc275)
 *   - GET /api/diag polling for the fault-diagnostics page
 *   - storage of calibration / OTA events that arrive as WS text JSON
 *
 * The UI only ever reads snapshots (scr_svc_get_*), all copies under a mutex.
 */
#ifndef SCR_SVC_H
#define SCR_SVC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- firmware staging + OTA ---------------------------------------------------*/
typedef enum {
    SVC_FW_C6 = 0,
    SVC_FW_TC = 1,
    SVC_FW_COUNT
} svc_fw_t;

typedef enum {
    SVC_STAGE_EMPTY = 0,    /* no SCFW header                          */
    SVC_STAGE_BAD,          /* header present but invalid / CRC fails  */
    SVC_STAGE_OK,           /* header + CRC verified                   */
} svc_stage_state_t;

typedef struct {
    svc_stage_state_t state;
    uint32_t size;
    uint32_t crc32;
    char     version[32];
    char     built[32];
    char     why[48];       /* reason when BAD                         */
} svc_stage_t;

typedef enum {
    SVC_OTA_IDLE = 0,
    SVC_OTA_VERIFY,         /* re-computing CRC over the staged image  */
    SVC_OTA_SEND,           /* streaming to the C6                      */
    SVC_OTA_WAIT_TC,        /* upload done, TC275 still writing/swapping */
    SVC_OTA_DONE,
    SVC_OTA_FAILED,
} svc_ota_phase_t;

typedef struct {
    svc_ota_phase_t phase;
    svc_fw_t target;
    uint8_t  pct;           /* S3 -> C6 transfer progress              */
    uint8_t  tc_pct;        /* TC275 write progress (otastatus)         */
    int      http_status;
    char     msg[96];       /* human-readable result / error           */
} svc_ota_t;

/* ---- C6 /api/diag -------------------------------------------------------------*/
typedef struct {
    bool     valid;         /* last poll parsed OK                     */
    int64_t  age_ms;        /* since last successful poll (-1 = never) */
    char     ver[24];
    char     state[16];
    char     slot[12];
    char     pair[10];
    uint32_t reset;
    uint32_t selfcheck;
    bool     coredump;
    bool     factory;
    uint32_t heap_min;
    uint32_t uptime_s;
    int      cli;
    uint32_t zs;
    bool     link_up;
    uint32_t link_rtt;
    uint32_t crc_err, fmt_err, rx, tx, busy;
    uint32_t crc_err_delta, fmt_err_delta;   /* growth since previous poll */
    int      imu;           /* -1 absent, 0 error, 1 ok               */
} svc_c6diag_t;

/* ---- calibration (TC275 DPT via C6, doc esp32c6_car 17) ---------------------------*/
typedef struct {
    bool     have_cal;      /* a {"t":"cal"} arrived since last start  */
    uint8_t  status;        /* 0 done, 1 estop-abort, 2 busy           */
    int      saved;         /* -1 unknown, 0 not saved, 1 written, 2 fail */
    int8_t   invert[4];
    int32_t  delta[4];
    int64_t  cal_started_ms;   /* 0 = no run pending               */

    bool     have_rec;
    uint8_t  rec_ver, rec_src, rec_crc_ok;
    uint8_t  pos[4];
    int8_t   rec_invert[4];
    int16_t  full_scale, wheel_dia;
    int64_t  rec_ms;

    /* live jog counts {"t":"jogcnt"}: per-motor (A..D) encoder delta since
     * the current jog press, 10 Hz while jogging */
    bool     have_jog;
    bool     jog_on;
    int32_t  jog_d[4];
    int64_t  jog_ms;
} svc_cal_t;

void scr_svc_start(void);

/* staging: cheap header read (no CRC), refreshed on request */
void scr_svc_get_stage(svc_fw_t t, svc_stage_t *out);
void scr_svc_rescan_stage(void);

/* OTA: returns false (and fills why) when a precondition fails */
bool scr_svc_ota_start(svc_fw_t t, char *why, int why_cap);
void scr_svc_get_ota(svc_ota_t *out);
/* True while a C6-target OTA makes an S3<->C6 link drop expected (upload
 * starving telemetry, or the C6 rebooting into an accepted image within the
 * reconnect grace window).  scr_ctrl keeps the RADIO LOST overlay quiet. */
bool scr_svc_ota_quiet_c6(void);

/* diag polling is only active while the diag page is visible */
void scr_svc_diag_poll_enable(bool on);
void scr_svc_diag_poll_now(void);
void scr_svc_get_c6diag(svc_c6diag_t *out);

/* calibration commands (WS binary, CTRL required) */
bool scr_svc_cal_rec_get(void);
bool scr_svc_cal_start_dir(void);
bool scr_svc_cal_rec_set(int16_t full_scale, int16_t wheel_dia);
bool scr_svc_cal_rec_clear(void);
bool scr_svc_clear_fault(void);
void scr_svc_get_cal(svc_cal_t *out);

/* WS text events routed from scr_link (runs in the websocket task) */
void scr_svc_on_ws_json(const char *type, const void *cjson_root);
/* WS dropped: an in-flight TC relay cannot report any more */
void scr_svc_on_ws_down(void);

#ifdef __cplusplus
}
#endif

#endif /* SCR_SVC_H */
