#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "sdkconfig.h"

#include "app_state.h"
#include "scr_settings.h"
#include "scr_link.h"
#include "scr_ctrl.h"
#include "proto/proto_frames.h"

typedef struct {
    bool radio_lost_raised;
    bool link_was_ok;       /* an established link was lost -> overlay is
                               meaningful; "never connected" stays in the
                               status bar instead (spec 102 semantics) */
    bool fault_raised;
    bool batt_warn_raised;
    bool batt_crit_raised;
} ctrl_flags_t;

static ctrl_flags_t s_flags;

static bool can_tx(void)
{
    scr_state_t st;
    app_state_snapshot(&st);
    return st.conn == SCR_CONN_CONNECTED && st.ctrl_role;
}

static void send_drive(int16_t v, int16_t w)
{
    if (!can_tx()) {
        return;
    }
    uint8_t data[4];
    proto_put_u16(&data[0], (uint16_t)v);
    proto_put_u16(&data[2], (uint16_t)w);

    uint8_t frame[PROTO_MAX_FRAME];
    size_t n = proto_build(PROTO_CMD_DRIVE, scr_link_next_seq(), data, sizeof(data),
                           frame, sizeof(frame));
    if (n > 0) {
        scr_link_send_bin(frame, n);
    }
}

static void send_emergency_stop(void)
{
    if (!can_tx()) {
        return;
    }
    uint8_t frame[PROTO_MAX_FRAME];
    /* legacy 0x32 EMERGENCY_STOP passes through to the TC275 (doc 02 section 3);
     * DRIVE 0,0 right behind it is the guaranteed stop either way */
    size_t n = proto_build(PROTO_CMD_EMERGENCY_STOP, scr_link_next_seq(), NULL, 0,
                           frame, sizeof(frame));
    if (n > 0) {
        scr_link_send_bin(frame, n);
    }
}

static int16_t clamp_i16(int32_t x)
{
    if (x > 32767)  return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

static void radio_lost_enter(void)
{
    if (s_flags.radio_lost_raised) {
        return;
    }
    s_flags.radio_lost_raised = true;
    app_alert_raise(SCR_ALERT_ID_RADIO_LOST, SCR_ALERT_CRITICAL,
                    "RADIO CONNECTION LOST",
                    "Vehicle stopped. Waiting for the C6 link to recover.");
    app_state_log(SCR_LOG_CRIT, "RADIO LOST - vehicle stop");
}

static void radio_lost_exit(void)
{
    if (!s_flags.radio_lost_raised) {
        return;
    }
    s_flags.radio_lost_raised = false;
    app_alert_clear(SCR_ALERT_ID_RADIO_LOST);
    app_state_log(SCR_LOG_INFO, "Radio recovered");
}

static void safety_watch(const scr_state_t *st)
{
    /* spec 102: radio lost must be loud and must say the vehicle stopped -
     * but only for a link that was actually established before, and only
     * after the loss is SUSTAINED: telemetry hiccups of a few hundred ms
     * (vehicle boot, AP broadcast pacing) must not strobe a full-screen
     * overlay on and off.  Stale data still shows "--" immediately (spec
     * 101); only the overlay waits CONFIG_SCR_ALERT_DEBOUNCE_MS. */
    bool link_ok = (st->conn == SCR_CONN_CONNECTED) && st->tele_fresh;
    static int64_t stale_since;
    if (link_ok) {
        s_flags.link_was_ok = true;
        stale_since = 0;
        radio_lost_exit();
    } else if (s_flags.link_was_ok) {
        int64_t now = esp_timer_get_time() / 1000;
        if (stale_since == 0) {
            stale_since = now;
        }
        if (now - stale_since >= CONFIG_SCR_ALERT_DEBOUNCE_MS) {
            radio_lost_enter();
        }
    }

    /* spec 62: vehicle fault with its source */
    if (st->fault_code != 0 && !s_flags.fault_raised) {
        s_flags.fault_raised = true;
        app_alert_raise(SCR_ALERT_ID_VEHICLE, SCR_ALERT_WARNING,
                        "VEHICLE FAULT", "TC275 fault code 0x%04x", st->fault_code);
        app_state_log(SCR_LOG_WARN, "Vehicle fault 0x%04x", st->fault_code);
    } else if (st->fault_code == 0 && s_flags.fault_raised) {
        s_flags.fault_raised = false;
        app_alert_clear(SCR_ALERT_ID_VEHICLE);
        app_state_log(SCR_LOG_INFO, "Vehicle fault cleared");
    }

    /* spec 56: battery low / critical */
    if (st->batt_pct > 0) {
        if (st->batt_pct <= CONFIG_SCR_BATT_CRIT_PCT && !s_flags.batt_crit_raised) {
            s_flags.batt_crit_raised = true;
            app_alert_raise(SCR_ALERT_ID_BATTERY, SCR_ALERT_CRITICAL,
                            "CRITICAL BATTERY", "Vehicle battery %u%%", st->batt_pct);
        } else if (st->batt_pct <= CONFIG_SCR_BATT_LOW_PCT &&
                   st->batt_pct > CONFIG_SCR_BATT_CRIT_PCT && !s_flags.batt_warn_raised) {
            s_flags.batt_warn_raised = true;
            app_alert_raise(SCR_ALERT_ID_BATTERY, SCR_ALERT_WARNING,
                            "LOW BATTERY", "Vehicle battery %u%%", st->batt_pct);
        } else if (st->batt_pct > CONFIG_SCR_BATT_LOW_PCT + 5 &&
                   (s_flags.batt_warn_raised || s_flags.batt_crit_raised)) {
            s_flags.batt_warn_raised = false;
            s_flags.batt_crit_raised = false;
            app_alert_clear(SCR_ALERT_ID_BATTERY);
        }
    }
}

static void ctrl_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_SCR_CTRL_RATE_HZ);

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        scr_state_t st;
        app_state_snapshot(&st);
        safety_watch(&st);

        /* gate: only a connected CTRL session may command (phone parity) */
        bool may_drive = (st.conn == SCR_CONN_CONNECTED) && st.ctrl_role &&
                         !st.stop_latch && !st.emerg_latch;

        int16_t v = 0, w = 0;
        if (may_drive && (st.joy_v != 0 || st.joy_w != 0)) {
            scr_settings_t set;
            scr_settings_get(&set);
            int pct;
            switch ((scr_mode_t)set.mode) {
                case SCR_MODE_ECO:   pct = CONFIG_SCR_MODE_ECO_PCT;   break;
                case SCR_MODE_SPORT: pct = CONFIG_SCR_MODE_SPORT_PCT; break;
                default:             pct = CONFIG_SCR_MODE_NORMAL_PCT; break;
            }
            v = clamp_i16((int32_t)st.joy_v * pct / 100);
            w = clamp_i16((int32_t)st.joy_w * pct / 100);
            send_drive(v, w);
        } else if (st.conn == SCR_CONN_CONNECTED && st.ctrl_role) {
            /* latched stop / emergency / zero input: keep the heartbeat fed
             * with a zero DRIVE so the TC275 heartbeat watchdog stays happy */
            send_drive(0, 0);
        }
        app_state_set_out(v, w);
    }
}

/* ---- public actions ----------------------------------------------------------*/
void scr_ctrl_start(void)
{
    memset(&s_flags, 0, sizeof(s_flags));
    /* core 1: Wi-Fi/lwip own core 0; sharing core 1 with the LVGL task at a
     * higher prio keeps the 30 ms control cadence immune to UI load */
    if (xTaskCreatePinnedToCore(ctrl_task, "scr_ctrl", 4096, NULL, 5, NULL, 1) != pdPASS) {
        app_state_log(SCR_LOG_CRIT, "ctrl task create failed");
    }
}

void scr_ctrl_stop_button(void)
{
    app_state_set_joy(0, 0);
    app_state_set_stop(true);
    /* stop immediately, don't wait for the next 33 ms tick */
    send_drive(0, 0);
    app_state_set_out(0, 0);
    app_state_log(SCR_LOG_NOTICE, "STOP");
}

void scr_ctrl_emergency(void)
{
    app_state_set_joy(0, 0);
    app_state_set_emerg(true);
    app_state_set_stop(true);
    send_emergency_stop();
    send_drive(0, 0);
    app_state_set_out(0, 0);
    app_alert_raise(SCR_ALERT_ID_EMERGENCY, SCR_ALERT_CRITICAL,
                    "EMERGENCY STOP",
                    "Inputs frozen. Release to recover - vehicle stays stopped.");
    app_state_log(SCR_LOG_CRIT, "EMERGENCY STOP");
}

void scr_ctrl_emergency_release(void)
{
    app_state_set_emerg(false);
    app_alert_clear(SCR_ALERT_ID_EMERGENCY);
    send_drive(0, 0);
    app_state_set_out(0, 0);
    /* stop latch stays on: motion requires an explicit new joystick input */
    app_state_log(SCR_LOG_NOTICE, "Emergency released");
}

void scr_ctrl_joystick_touch(void)
{
    scr_state_t st;
    app_state_snapshot(&st);
    if (st.stop_latch && !st.emerg_latch) {
        app_state_set_stop(false);
        app_state_log(SCR_LOG_INFO, "STOP released, joystick live");
    }
}

void scr_ctrl_control_lost(void)
{
    app_state_set_joy(0, 0);
    app_state_set_stop(true);
    app_state_set_out(0, 0);
}
