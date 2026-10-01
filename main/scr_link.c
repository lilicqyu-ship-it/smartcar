/*
 * scr_link.c - Wi-Fi STA -> C6 softAP -> WebSocket -> proto v2
 *
 * Protocol mirror of esp32c6_car (all documented in the esp32c6_car/doc set):
 *   frame : AA 55 VER=02 CMD SEQ LEN DATA[<=64] CRC16-CCITT-FALSE
 *   drive : 0x50 {i16 v mm/s, i16 omega deg/s}, doubles as TC275 heartbeat
 *   tele  : 0x41 38 B LE payload, broadcast to every WS session
 *   text  : hello{role,ver,tc,...} / tc{on} / pong / err{e}; we send {"t":"ping"}
 *   pair  : POST /api/pair during the car-side window -> {ok, token}
 */
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"

#include "app_state.h"
#include "scr_settings.h"
#include "scr_link.h"
#include "scr_ctrl.h"
#include "scr_svc.h"
#include "proto/proto_frames.h"

#define MON_PERIOD_MS       250     /* monitor task tick            */

static const char *TAG = "scr_link";

typedef struct {
    esp_websocket_client_handle_t ws;
    volatile bool ws_running;
    volatile bool wifi_up;
    volatile bool got_ip;
    volatile bool pair_req;
    volatile bool c6_ver_req;           /* UI asked: re-query C6 version (/api/health) */
    volatile bool tc_ver_req;           /* UI asked: {"t":"tcver"} -> C6 -> SPI -> TC275 */
    uint8_t ws_fail_run;                /* consecutive WS disconnects w/o a connect */

    proto_parser_t parser;

    uint8_t seq;                        /* per-session frame counter    */
    volatile int sock;                  /* TCP socket to the C6, -1 = none */
    uint32_t tx_skip;                   /* frames skipped: socket full  */

    int64_t last_rx_ms;
    int64_t ping_sent_ms;
    bool    ping_pending;
    bool    had_ctrl;                   /* this session held CTRL (hello role) */

    /* 1 s window statistics */
    uint32_t tx_cnt, rx_cnt, lost_cnt;
    uint32_t prev_seq;
    bool     prev_valid;
    uint16_t lat_min, lat_max;
} link_t;

static link_t s_link;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---- Wi-Fi events ------------------------------------------------------------*/
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        /* console-only diagnosis (NO_AP_FOUND vs AUTH_FAIL vs ...): the event
         * ring must not drown in retry cycles while the car is simply off */
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", ev ? ev->reason : -1);
        s_link.wifi_up = false;
        app_state_set_conn(SCR_CONN_NONE);
        /* event-driven retry: fires only when truly disconnected, so it can
           never collide with an attempt that is still connecting */
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_link.wifi_up = true;      /* opens the monitor's WS start path */
        s_link.got_ip = true;
        app_state_set_conn(SCR_CONN_CONNECTING);
        app_state_log(SCR_LOG_INFO, "WiFi up");
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    scr_settings_t set;
    scr_settings_get(&set);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, set.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, set.pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    /* driving link: no modem power save, or latency jumps every beacon */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    app_state_boot_mark_radio();
    app_state_log(SCR_LOG_INFO, "Radio start SSID=%s", set.ssid);
}

/* ---- WebSocket ------------------------------------------------------------------*/
static void ws_apply_hello(const cJSON *root)
{
    const cJSON *role     = cJSON_GetObjectItem(root, "role");
    const cJSON *any_ctrl = cJSON_GetObjectItem(root, "ctrl");
    const cJSON *ver      = cJSON_GetObjectItem(root, "ver");
    const cJSON *tc       = cJSON_GetObjectItem(root, "tc");

    bool ctrl = cJSON_IsString(role) && strcmp(role->valuestring, "ctrl") == 0;
    if (s_link.had_ctrl && !ctrl) {
        /* the C6 handed control to another client while we were connected:
         * latch a stop and zero the stick so nothing keeps driving (spec 104) */
        scr_ctrl_control_lost();
    }
    s_link.had_ctrl = ctrl;

    if (ctrl) {
        app_state_set_ctrl_role(true);
        app_state_log(SCR_LOG_INFO, "Control owner: S3");
    } else if (cJSON_IsBool(any_ctrl) && cJSON_IsTrue(any_ctrl)) {
        /* hello.ctrl = "some session holds CTRL" -> it is not us: phone took
         * over (spec 103/104: show WEB MASTER, joystick is gated off) */
        app_state_set_ctrl_role(false);
        app_state_set_owner(SCR_OWNER_WEB);
        app_state_log(SCR_LOG_INFO, "Control owner: WEB");
    } else {
        app_state_set_ctrl_role(false);
        app_state_set_owner(SCR_OWNER_NONE);
        app_state_log(SCR_LOG_INFO, "No control owner");
    }
    if (cJSON_IsString(ver)) {
        app_state_set_c6_fw(ver->valuestring);
    }
    /* hello.tc is a placeholder STRING ("down") on the C6 - http_server has no
     * link visibility - and the C6 broadcasts the real {"t":"tc","on":..}
     * just BEFORE the hello (ws_sess_set_ws -> bridge_notify_clients).
     * Applying the string as false overwrote that true and left tc_on stuck
     * off on a stable link (SPI wire DOWN, TC275 version tap refused).
     * Only a real bool may set the vehicle state. */
    if (cJSON_IsBool(tc)) {
        app_state_set_tc(cJSON_IsTrue(tc));
    }
}

static void ws_handle_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        return;
    }
    const cJSON *t = cJSON_GetObjectItem(root, "t");
    if (cJSON_IsString(t)) {
        if (strcmp(t->valuestring, "hello") == 0) {
            ws_apply_hello(root);
        } else if (strcmp(t->valuestring, "tc") == 0) {
            const cJSON *on = cJSON_GetObjectItem(root, "on");
            bool up = cJSON_IsTrue(on);
            app_state_set_tc(up);
            app_state_log(SCR_LOG_INFO, up ? "Vehicle link up" : "Vehicle link down");
        } else if (strcmp(t->valuestring, "pong") == 0) {
            if (s_link.ping_pending) {
                int64_t rtt = now_ms() - s_link.ping_sent_ms;
                if (rtt >= 0 && rtt < 60000) {
                    app_state_set_latency((uint16_t)rtt);
                    if (s_link.lat_min == 0 || rtt < s_link.lat_min) {
                        s_link.lat_min = (uint16_t)rtt;
                    }
                    if (rtt > s_link.lat_max) {
                        s_link.lat_max = (uint16_t)rtt;
                    }
                }
                s_link.ping_pending = false;
            }
        } else if (strcmp(t->valuestring, "cal") == 0 ||
                   strcmp(t->valuestring, "rec") == 0 ||
                   strcmp(t->valuestring, "otastatus") == 0 ||
                   strcmp(t->valuestring, "otaswap") == 0 ||
                   strcmp(t->valuestring, "otaerror") == 0) {
            /* calibration results / OTA progress: owned by the service module */
            if (t->valuestring[0] == 'c' || t->valuestring[0] == 'r') {
                ESP_LOGI(TAG, "CALJSON %.*s", len > 200 ? 200 : len, data);
            }
            scr_svc_on_ws_json(t->valuestring, root);
        } else if (strcmp(t->valuestring, "tcver") == 0) {
            /* TC275 version beacon: {"t":"tcver","app":"...","sbl":"..."} */
            const cJSON *a = cJSON_GetObjectItem(root, "app");
            const cJSON *b = cJSON_GetObjectItem(root, "sbl");
            app_state_set_tc_ver(cJSON_IsString(a) ? a->valuestring : "",
                                 cJSON_IsString(b) ? b->valuestring : "");
        } else if (strcmp(t->valuestring, "err") == 0) {
            const cJSON *e = cJSON_GetObjectItem(root, "e");
            if (cJSON_IsString(e) && strcmp(e->valuestring, "auth") == 0) {
                /* role gate rejected us: the C6 gave CTRL to another client */
                if (s_link.had_ctrl) {
                    s_link.had_ctrl = false;
                    scr_ctrl_control_lost();
                }
                app_state_set_ctrl_role(false);
                app_state_set_owner(SCR_OWNER_NONE);
                app_state_log(SCR_LOG_WARN, "Control rejected (auth)");
            }
        }
    }
    cJSON_Delete(root);
}

static void ws_handle_telemetry(const uint8_t *data, int len)
{
    proto_telemetry_t t;
    if (proto_telemetry_decode(data, (size_t)len, &t) != 0) {
        return;
    }
    /* E2E sequence gap -> telemetry loss estimate (spec 28) */
    if (s_link.prev_valid) {
        uint32_t d = t.seq - s_link.prev_seq;
        if (d > 1 && d < 1000) {
            s_link.lost_cnt += d - 1;
        }
    }
    s_link.prev_seq = t.seq;
    s_link.prev_valid = true;

    s_link.rx_cnt++;
    app_state_set_telemetry(&t);

    /* console-only speed trace (2 Hz while anything moves): per-side target vs
     * measured mm/s straight off the wire, so a wrong encoder sign or a dead
     * side is visible without the DIAGNOSE page */
    static int64_t s_tele_log_ms;
    int64_t now = now_ms();
    if ((t.v_target_l || t.v_target_r || t.v_meas_l || t.v_meas_r) &&
        now - s_tele_log_ms >= 500) {
        s_tele_log_ms = now;
        ESP_LOGI(TAG, "TELE tgt L=%d R=%d | meas L=%d R=%d | st=%u flt=0x%04X",
                 t.v_target_l, t.v_target_r, t.v_meas_l, t.v_meas_r,
                 (unsigned)t.state, (unsigned)t.fault_code);
    }
}

/* ---- latency: disable Nagle on the C6 connection ------------------------------------
 * A browser sets TCP_NODELAY on its WebSocket; esp_websocket_client does not.
 * With Nagle on, each 12-byte DRIVE frame waits for the ACK of the previous
 * one, and lwIP on the C6 delays ACKs up to one TCP timer tick - a joystick
 * move then reaches the car 100-200 ms late (the "S3 feels laggy vs the phone"
 * symptom).  The client exposes no socket getter, so walk the lwIP socket table
 * and mark every TCP socket whose peer is the C6. */
static void link_set_nodelay(void)
{
    struct sockaddr_in peer;
    ip4_addr_t c6;
    if (!ip4addr_aton(CONFIG_SCR_C6_IP, &c6)) {
        return;
    }
    s_link.sock = -1;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        socklen_t len = sizeof(peer);
        if (getpeername(fd, (struct sockaddr *)&peer, &len) == 0 &&
            peer.sin_family == AF_INET && peer.sin_addr.s_addr == c6.addr) {
            int one = 1;
            (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            s_link.sock = fd;
        }
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            link_set_nodelay();
            s_link.ws_running = true;
            s_link.ws_fail_run = 0;
            s_link.last_rx_ms = now_ms();
            app_state_set_conn(SCR_CONN_CONNECTED);
            app_state_log(SCR_LOG_INFO, "WS connected");
            /* mirror the phone page: an initial DRIVE 0,0 resyncs the chain */
            uint8_t f[PROTO_MAX_FRAME];
            size_t n = proto_build(PROTO_CMD_DRIVE, scr_link_next_seq(),
                                   (const uint8_t *)"\0\0\0\0", 4, f, sizeof(f));
            if (n > 0) {
                scr_link_send_bin(f, n);
            }
            break;
        }
        case WEBSOCKET_EVENT_DISCONNECTED:
            s_link.ws_running = false;
            s_link.sock = -1;
            s_link.had_ctrl = false;
            app_state_set_conn(SCR_CONN_NONE);
            app_state_set_ctrl_role(false);
            app_state_set_owner(SCR_OWNER_NONE);
            app_state_set_tc(false);
            scr_svc_on_ws_down();
            app_state_log(SCR_LOG_WARN, "WS disconnected");
            /* C6 rebooted under us: its softAP forgot this station, but the
             * STA may not notice for a long time (seen 09-30/10-01: WS connect
             * timeouts every 8 s, no Wi-Fi DISCONNECTED event).  After 3 WS
             * failures in a row, drop the association; the DISCONNECTED
             * handler reconnects from scratch. */
            if (++s_link.ws_fail_run >= 3 && s_link.wifi_up) {
                s_link.ws_fail_run = 0;
                ESP_LOGW(TAG, "WS keeps failing: forcing Wi-Fi re-association");
                esp_wifi_disconnect();
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            if (ev->data_len <= 0) {
                break;
            }
            s_link.last_rx_ms = now_ms();
            if (ev->op_code == 0x02) {          /* binary: proto v2 frames */
                proto_frame_t out;
                for (int i = 0; i < ev->data_len; i++) {
                    proto_rx_ev_t e = proto_parser_feed(&s_link.parser,
                                                        (uint8_t)ev->data_ptr[i], &out);
                    if (e == PROTO_RX_FRAME && out.cmd == PROTO_CMD_TELEMETRY) {
                        ws_handle_telemetry(out.data, out.len);
                    }
                }
            } else if (ev->op_code == 0x01) {   /* text: control plane JSON */
                ws_handle_text(ev->data_ptr, ev->data_len);
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            s_link.ws_running = false;
            break;
        default:
            break;
    }
}

static void ws_start(void)
{
    scr_settings_t set;
    scr_settings_get(&set);

    char uri[128];
    if (set.token[0] != '\0') {
        snprintf(uri, sizeof(uri), "ws://%s/ws?token=%s", CONFIG_SCR_C6_IP, set.token);
    } else {
        snprintf(uri, sizeof(uri), "ws://%s/ws", CONFIG_SCR_C6_IP);
    }

    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .buffer_size = 1024,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 3000,
        .task_stack = 6144,
        /* doc/08 §2: network work stays on core 0 (core 1 = ctrl + LVGL) */
        .task_prio = 5,
        .task_core_id_set = true,
        .task_core_id = 0,
    };
    s_link.ws = esp_websocket_client_init(&cfg);
    if (s_link.ws == NULL) {
        app_state_log(SCR_LOG_CRIT, "WS init failed");
        return;
    }
    esp_websocket_register_events(s_link.ws, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);
    esp_websocket_client_start(s_link.ws);
}

static void ws_restart(void)
{
    app_state_log(SCR_LOG_WARN, "WS restart");
    if (s_link.ws) {
        esp_websocket_client_stop(s_link.ws);
        esp_websocket_client_destroy(s_link.ws);
        s_link.ws = NULL;
    }
    s_link.ws_running = false;
    s_link.prev_valid = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    ws_start();
}

/* ---- C6 version query: GET /api/health -> {"up":true,"ver":"...",...} -----------------*/
/* Existing C6 endpoint (esp32c6_car http_server.c), so no C6 change is needed.
 * Runs in the monitor task; 1.5 s cap keeps the WS watchdog/ping path alive. */
static void c6_ver_do(void)
{
    char url[64];
    snprintf(url, sizeof(url), "http://%s/api/health", CONFIG_SCR_C6_IP);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 1500,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) {
        return;
    }
    char body[256] = { 0 };
    int body_len = 0;
    esp_err_t err = esp_http_client_open(h, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(h);
        while (body_len < (int)sizeof(body) - 1) {
            int r = esp_http_client_read(h, body + body_len, sizeof(body) - 1 - body_len);
            if (r <= 0) {
                break;
            }
            body_len += r;
        }
    }
    int status = esp_http_client_get_status_code(h);
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "C6 version query failed (err=%s status=%d)", esp_err_to_name(err), status);
        return;
    }
    cJSON *root = cJSON_ParseWithLength(body, body_len);
    const cJSON *ver = root ? cJSON_GetObjectItem(root, "ver") : NULL;
    if (cJSON_IsString(ver)) {
        app_state_set_c6_fw(ver->valuestring);
        ESP_LOGI(TAG, "C6 version refreshed: %s", ver->valuestring);
    }
    cJSON_Delete(root);
}

/* ---- pairing (doc esp32c6_car 06) ------------------------------------------------------*/
static void pair_do(void)
{
    app_state_set_pair_status("Requesting pair window...");

    char url[64];
    snprintf(url, sizeof(url), "http://%s/api/pair", CONFIG_SCR_C6_IP);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 6000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) {
        app_state_set_pair_status("HTTP init failed");
        return;
    }

    char body[128] = { 0 };
    int body_len = 0;
    esp_err_t err = esp_http_client_open(h, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(h);
        char chunk[64];
        while (body_len < (int)sizeof(body) - 1) {
            int r = esp_http_client_read(h, chunk, sizeof(chunk));
            if (r <= 0) {
                break;
            }
            memcpy(body + body_len, chunk, r);
            body_len += r;
        }
        body[body_len] = '\0';
    }
    int status = esp_http_client_get_status_code(h);
    esp_http_client_close(h);
    esp_http_client_cleanup(h);

    if (err != ESP_OK) {
        app_state_set_pair_status("No connection to C6");
        return;
    }

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    if (root != NULL && status == 200) {
        const cJSON *ok = cJSON_GetObjectItem(root, "ok");
        const cJSON *tok = cJSON_GetObjectItem(root, "token");
        if (cJSON_IsTrue(ok) && cJSON_IsString(tok)) {
            scr_settings_set_token(tok->valuestring);
            app_state_set_pair_status("Paired. Reconnecting...");
            app_state_log(SCR_LOG_INFO, "Pairing ok");
            cJSON_Delete(root);
            ws_restart();
            return;
        }
    }
    cJSON_Delete(root);

    /* failure paths documented in esp32c6_car doc 06 section 3: tell the user what
     * to do next instead of a bare error (spec 41) */
    const char *hint;
    switch (status) {
        case 403: hint = "No pair window. Press car button 3 s first"; break;
        case 409: hint = "Vehicle busy: another controller paired"; break;
        case 504: hint = "Timeout. Check vehicle, then retry"; break;
        case 503: hint = "Vehicle link down (TC275 offline)"; break;
        default:  hint = "Pair failed. Check C6 and retry"; break;
    }
    app_state_set_pair_status(hint);
    app_state_log(SCR_LOG_WARN, "Pair failed: %s", hint);
}

/* ---- monitor task --------------------------------------------------------------------*/
static scr_qual_t quality_from_rssi(int8_t rssi)
{
    if (rssi >= CONFIG_SCR_RSSI_EXCELLENT) return SCR_QUAL_EXCELLENT;
    if (rssi >= CONFIG_SCR_RSSI_GOOD)      return SCR_QUAL_GOOD;
    if (rssi >= CONFIG_SCR_RSSI_FAIR)      return SCR_QUAL_FAIR;
    if (rssi >= CONFIG_SCR_RSSI_WEAK)      return SCR_QUAL_WEAK;
    return SCR_QUAL_CRITICAL;
}

static void housekeeping_1hz(void)
{
    /* RSSI + channel (spec 24/25: number AND graphical quality) */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        app_state_set_wifi(ap.rssi, ap.primary);
        app_state_set_quality(quality_from_rssi(ap.rssi));
    }

    /* JSON ping -> pong RTT (S3 <-> C6 latency, spec 27) */
    if (s_link.ws_running) {
        s_link.ping_pending = true;
        s_link.ping_sent_ms = now_ms();
        scr_link_send_text("{\"t\":\"ping\"}");
    }

    if (s_link.tx_skip) {
        ESP_LOGW(TAG, "skipped %lu frame(s): socket busy", (unsigned long)s_link.tx_skip);
        s_link.tx_skip = 0;
    }

    /* rates + loss over the past second (spec 28) */
    uint16_t tx = (uint16_t)s_link.tx_cnt;
    uint16_t rx = (uint16_t)s_link.rx_cnt;
    uint32_t lost = s_link.lost_cnt;
    s_link.tx_cnt = 0;
    s_link.rx_cnt = 0;
    s_link.lost_cnt = 0;
    app_state_set_rates(tx, rx);

    uint32_t total = lost + rx;
    uint16_t loss_x10 = (uint16_t)((total > 0) ? (lost * 1000u + total / 2u) / total : 0);
    if (rx == 0 && s_link.prev_valid) {
        loss_x10 = 1000;    /* expected traffic but nothing arrived */
    }
    app_state_set_loss(loss_x10);
}

static void link_monitor_task(void *arg)
{
    int64_t last_1hz = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MON_PERIOD_MS));
        int64_t now = now_ms();

        if (!s_link.wifi_up) {
            continue;       /* reconnect is event-driven (wifi_event_handler) */
        }

        if (s_link.got_ip && !s_link.ws_running && s_link.ws == NULL) {
            ws_start();
        }

        if (s_link.ws_running) {
            /* silence watchdog: half-open socket reconnect (mirrors app.js) */
            if (now - s_link.last_rx_ms > CONFIG_SCR_RX_WATCHDOG_MS) {
                app_state_log(SCR_LOG_WARN, "WS silence, reconnect");
                ws_restart();
                continue;
            }
        }

        if (s_link.pair_req) {
            s_link.pair_req = false;
            pair_do();
        }

        if (s_link.c6_ver_req) {
            s_link.c6_ver_req = false;
            c6_ver_do();
        }

        if (s_link.tc_ver_req) {
            s_link.tc_ver_req = false;
            /* C6 relays it to the TC275 as SPI DIAG 0x53/0x24; the answer comes
             * back as the normal {"t":"tcver"} beacon (ws_handle_text) */
            if (!scr_link_send_text("{\"t\":\"tcver\"}")) {
                ESP_LOGW(TAG, "tcver request not sent");
            }
        }

        if (now - last_1hz >= 1000) {
            last_1hz = now;
            housekeeping_1hz();
        }
    }
}

/* ---- public API -----------------------------------------------------------------------*/
void scr_link_start(void)
{
    memset(&s_link, 0, sizeof(s_link));
    s_link.sock = -1;
    proto_parser_init(&s_link.parser);
    wifi_init();

    if (xTaskCreatePinnedToCore(link_monitor_task, "scr_link", 6144, NULL, 4, NULL, 0) != pdPASS) {
        app_state_log(SCR_LOG_CRIT, "link task create failed");
    }
}

/* Zero-wait "can this frame go out now?".  esp_websocket_client treats a
 * write that cannot complete within its timeout as a transport error and
 * ABORTS the connection ("transport_poll_write(0)" -> RADIO LOST).  With
 * TCP_NODELAY and event-driven DRIVE the send buffer can be momentarily full
 * during a Wi-Fi retry burst; skipping one frame (the next one is <=33 ms away
 * and carries the newer stick value) is harmless, dropping the link is not. */
static bool sock_writable(void)
{
    int fd = s_link.sock;
    if (fd < 0) {
        return true;        /* unknown socket: let the client decide */
    }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = { 0, 0 };
    return select(fd + 1, NULL, &w, NULL, &tv) > 0 && FD_ISSET(fd, &w);
}

bool scr_link_send_bin(const uint8_t *data, size_t len)
{
    /* is_connected is a cheap atomic check: during reconnect attempts the
     * client task holds the internal lock, and a blocking send here would
     * stall the 30 Hz ctrl task on core 1 - starving the LVGL bounce-buffer
     * feed, which shows up as screen jitter.  A dropped DRIVE frame is
     * harmless: the next tick (33 ms) resends. */
    if (data == NULL || len == 0 || !s_link.ws_running || s_link.ws == NULL ||
        !esp_websocket_client_is_connected(s_link.ws)) {
        return false;
    }
    /* 5 ms: the TX lock is only ever held for one ~20 B frame write (ping
     * text, library PING/PONG), so a short wait turns the periodic collision
     * into a sub-ms delay instead of a dropped DRIVE + error log line, while
     * still never parking the ctrl task on a stalled socket. */
    if (!sock_writable()) {
        s_link.tx_skip++;
        return false;
    }
    /* 150 ms: esp_websocket_client uses this timeout for the socket write as
     * well as the lock, and ABORTS the connection when a write cannot finish
     * in time.  5 ms is shorter than one Wi-Fi retransmission burst and caused
     * a RADIO LOST every few minutes (soak 09-30: "Poll timeout ... 5 ms").
     * Waiting only parks the ctrl task (it sleeps, LVGL keeps core 1); a
     * socket stalled for >150 ms is a genuinely dead link. */
    int r = esp_websocket_client_send_bin(s_link.ws, (const char *)data,
                                          (int)len, pdMS_TO_TICKS(150));
    if (r > 0) {
        s_link.tx_cnt++;
        return true;
    }
    return false;
}

bool scr_link_send_text(const char *text)
{
    if (text == NULL || !s_link.ws_running || s_link.ws == NULL ||
        !esp_websocket_client_is_connected(s_link.ws)) {
        return false;
    }
    /* Only the 1 Hz monitor task sends text (ping/pair control), so it may
     * wait briefly for the TX lock the 30 Hz DRIVE sender holds for a few
     * hundred us.  With timeout 0 the ping lost that race every few seconds
     * ("Could not lock ws-client within 0 timeout"), the pong never came and
     * the RTT/loss readout jumped - and each miss printed an error line from
     * the send path.  20 ms stays far below the ctrl period of the C6 side. */
    if (!sock_writable()) {
        return false;       /* ping/pair retried by their callers */
    }
    int r = esp_websocket_client_send_text(s_link.ws, text,
                                           (int)strlen(text), pdMS_TO_TICKS(150));
    return r > 0;
}

uint8_t scr_link_next_seq(void)
{
    return ++s_link.seq;
}

void scr_link_request_pair(void)
{
    s_link.pair_req = true;
}

void scr_link_request_c6_ver(void)
{
    s_link.c6_ver_req = true;
}

void scr_link_request_tc_ver(void)
{
    s_link.tc_ver_req = true;
}

void scr_link_apply_wifi(void)
{
    scr_settings_t set;
    scr_settings_get(&set);

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, set.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, set.pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;

    s_link.got_ip = false;
    s_link.wifi_up = false;
    app_state_set_conn(SCR_CONN_CONNECTING);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_disconnect();      /* DISCONNECTED handler reconnects with the
                                   new credentials */
    app_state_log(SCR_LOG_INFO, "WiFi config applied");
}
