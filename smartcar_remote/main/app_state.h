/*
 * app_state.h - Single Source of Truth for the remote controller (spec 98-101)
 *
 * Radio layer -> protocol layer -> vehicle state -> UI state -> screen.
 * Every page (Home/Vehicle/Radio/Diagnostics) reads the same snapshot from
 * here; nothing keeps a private copy of RSSI/speed/... (spec 100).
 *
 * All data carries freshness information: stale telemetry shows "--" instead
 * of posing as live data (spec 101).  Telemetry that cannot be measured on
 * this hardware simply does not exist here - nothing is fabricated (spec 95.7).
 */
#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "proto/proto_frames.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits ----------------------------------------------------------------*/
#define SCR_EVENT_TEXT_MAX   48
#define SCR_EVENT_RING_LEN   32
#define SCR_ALERT_TITLE_MAX  32
#define SCR_ALERT_MSG_MAX    64
#define SCR_FW_STR_MAX       16

/* full-deflection constants, mirrors the phone control page (esp32c6_car app.js) */
#define SCR_DRIVE_V_MAX      600    /* mm/s at full joystick deflection  */
#define SCR_DRIVE_W_MAX      300    /* deg/s at full joystick deflection */

/* alert ids - one condition, one id; a new id re-shows the overlay */
#define SCR_ALERT_ID_RADIO_LOST  0x524C4F53u   /* "RLOS" */
#define SCR_ALERT_ID_EMERGENCY   0x454D5253u   /* "EMRS" */
#define SCR_ALERT_ID_VEHICLE     0x56464154u   /* "VFAT" */
#define SCR_ALERT_ID_BATTERY     0x4241544Cu   /* "BATL" */
#define SCR_ALERT_ID_PAIR        0x50415247u   /* "PARG" */

/* ---- enums (spec 12 / 26 / 33 / 59 / 21) -----------------------------------*/
typedef enum {
    SCR_CONN_NONE = 0,
    SCR_CONN_CONNECTING,
    SCR_CONN_CONNECTED,
} scr_conn_t;

typedef enum {
    SCR_OWNER_NONE = 0,     /* NO CONTROL (spec 12/103)  */
    SCR_OWNER_S3,           /* S3 MASTER                 */
    SCR_OWNER_WEB,          /* WEB MASTER                */
} scr_owner_t;

typedef enum {
    SCR_QUAL_UNKNOWN = 0,
    SCR_QUAL_EXCELLENT,
    SCR_QUAL_GOOD,
    SCR_QUAL_FAIR,
    SCR_QUAL_WEAK,
    SCR_QUAL_CRITICAL,
} scr_qual_t;

typedef enum {
    SCR_MODE_ECO = 0,
    SCR_MODE_NORMAL,
    SCR_MODE_SPORT,
} scr_mode_t;

typedef enum {
    SCR_LOG_INFO = 0,
    SCR_LOG_NOTICE,
    SCR_LOG_WARN,
    SCR_LOG_CRIT,
} scr_log_level_t;

typedef enum {
    SCR_ALERT_NONE = 0,
    SCR_ALERT_INFO,
    SCR_ALERT_NOTICE,
    SCR_ALERT_WARNING,
    SCR_ALERT_CRITICAL,
} scr_alert_level_t;

/* system state machine, spec 59 */
typedef enum {
    SCR_SYS_BOOT = 0,
    SCR_SYS_CONNECTING,
    SCR_SYS_READY,
    SCR_SYS_CONTROL,
    SCR_SYS_WARNING,
    SCR_SYS_FAULT,
    SCR_SYS_STOPPED,
    SCR_SYS_EMERGENCY,
} scr_sys_t;

typedef struct {
    scr_alert_level_t level;
    uint32_t id;
    char     title[SCR_ALERT_TITLE_MAX];
    char     msg[SCR_ALERT_MSG_MAX];
} scr_alert_t;

typedef struct {
    int64_t         ts_ms;              /* uptime when recorded */
    scr_log_level_t level;
    char            text[SCR_EVENT_TEXT_MAX];
} scr_event_t;

/* ---- the snapshot -----------------------------------------------------------*/
typedef struct {
    /* link (S3 <-> C6) */
    scr_conn_t   conn;
    bool         ctrl_role;         /* C6 hello: this session holds CTRL     */
    bool         tc_on;             /* C6 <-> TC275 link up (hello/tc json)  */
    bool         tele_fresh;        /* telemetry inside the timeout window   */
    int8_t       rssi;
    uint8_t      channel;
    uint16_t     lat_ms;            /* S3<->C6 WS round trip, measured 1 Hz  */
    uint16_t     lat_min;
    uint16_t     lat_max;
    uint16_t     tx_rate;           /* DRIVE frames per second               */
    uint16_t     rx_rate;           /* telemetry frames per second           */
    uint16_t     loss_pct_x10;      /* telemetry loss, 0.1 % units (1000=100%) */
    uint32_t     tele_seq;          /* last telemetry E2E sequence           */
    scr_qual_t   quality;
    char         c6_fw[SCR_FW_STR_MAX];
    char         tc_app_ver[32];    /* "APPFW tc275_car vX.Y.Z" from {"t":"tcver"} */
    char         tc_sbl_ver[32];    /* "SBLFW tc275_sbl vX.Y.Z", "" = SBL absent   */
    uint32_t     c6_fw_seq;         /* +1 on every C6 version report (hello / health) */
    uint32_t     tc_ver_seq;        /* +1 on every {"t":"tcver"} beacon              */
    uint16_t     link_rtt_ms;       /* C6<->TC275 link RTT, from telemetry   */
    uint8_t      link_err_rate;     /* C6<->TC275 error rate, 0.1 % units    */
    char         pair_status[64];   /* pairing page feedback                 */

    /* vehicle telemetry (proto 0x41, 38 B LE) */
    int32_t      speed_mm_s;        /* EMA of (v_meas_l + v_meas_r) / 2      */
    int16_t      v_target_l, v_target_r;
    int16_t      v_meas_l, v_meas_r;
    uint8_t      batt_pct;
    uint16_t     batt_mv;
    uint32_t     odo_session_mm;
    uint32_t     odo_total_mm;
    uint16_t     fault_code;
    uint8_t      veh_state;         /* TC275 mission state, raw code         */
    uint32_t     tc_fw_ver;
    uint8_t      hw_rev;

    /* control */
    scr_owner_t  owner;
    scr_mode_t   mode;
    int16_t      joy_v, joy_w;      /* raw joystick input                    */
    int16_t      out_v, out_w;      /* what the ctrl task actually sent      */
    bool         stop_latch;        /* STOP pressed - vehicle held stopped   */
    bool         emerg_latch;       /* emergency stop - explicit release     */

    /* boot self check (P0 page) */
    bool         boot_lcd;
    bool         boot_touch;
    bool         boot_radio;
    bool         boot_sys;

    /* system-level alert overlay (spec 20-22) */
    scr_alert_t  alert;
    bool         alert_ack;         /* user acknowledged the current alert   */

    /* misc */
    uint32_t     uptime_ms;
} scr_state_t;

/* ---- lifecycle ---------------------------------------------------------------*/
void app_state_init(void);

/* Atomic snapshot copy.  Cheap enough to call at 10-30 Hz from the UI. */
void app_state_snapshot(scr_state_t *out);

/* Pure derivation of the system state from a snapshot (spec 59). */
scr_sys_t app_state_derive(const scr_state_t *s);

/* ---- link side setters (called from scr_link) ---------------------------------*/
void app_state_set_conn(scr_conn_t c);
void app_state_set_ctrl_role(bool ctrl);
void app_state_set_owner(scr_owner_t o);    /* WEB MASTER display (spec 103/104) */
void app_state_set_tc(bool on);
void app_state_set_wifi(int8_t rssi, uint8_t ch);
void app_state_set_latency(uint16_t ms);
void app_state_set_rates(uint16_t tx, uint16_t rx);
void app_state_set_loss(uint16_t pct_x10);
void app_state_set_quality(scr_qual_t q);
void app_state_set_c6_fw(const char *fw);
void app_state_set_tc_ver(const char *app, const char *sbl);
void app_state_set_telemetry(const proto_telemetry_t *t);
void app_state_set_pair_status(const char *txt);    /* pairing page feedback  */

/* ---- control side (called from scr_ctrl) --------------------------------------*/
void app_state_set_joy(int16_t v, int16_t w);
void app_state_set_out(int16_t v, int16_t w);
void app_state_set_stop(bool on);
void app_state_set_emerg(bool on);
void app_state_set_mode(scr_mode_t m);

/* ---- boot self check (P0 page, one mark per stage) ----------------------------*/
void app_state_boot_mark_lcd(void);
void app_state_boot_mark_touch(void);
void app_state_boot_mark_radio(void);
void app_state_boot_mark_sys(void);

/* ---- event history (spec 83) ------------------------------------------------------*/
void app_state_log(scr_log_level_t lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int  app_state_events_get(scr_event_t *out, int cap);   /* newest first */

/* ---- events + alerts --------------------------------------------------------------
 * Only safety-relevant events raise a full-screen overlay; ordinary info stays
 * in the status bar.  Alerts live in per-id slots so simultaneous alerts do
 * not clobber each other; the overlay shows the highest severity.  A slot is
 * shown until its condition clears (app_alert_clear with the same id) or the
 * user acknowledges it. */
void app_alert_raise(uint32_t id, scr_alert_level_t lvl,
                     const char *title, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void app_alert_clear(uint32_t id);
void app_alert_ack(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATE_H */
