#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "scr_settings.h"
#include "app_state.h"

static const char *TAG = "scr_set";

static const char *NS = "scr";

static scr_settings_t s_set;
static SemaphoreHandle_t s_mtx;

static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        /* keep the default already in out */
    }
}

void scr_settings_init(void)
{
    s_mtx = xSemaphoreCreateMutex();

    strlcpy(s_set.ssid, CONFIG_SCR_AP_SSID, sizeof(s_set.ssid));
    strlcpy(s_set.pass, CONFIG_SCR_AP_PASS, sizeof(s_set.pass));
    s_set.token[0]      = '\0';
    s_set.mode          = SCR_MODE_NORMAL;
    s_set.deadzone_pct  = 8;    /* spec 15: bench default, tune while driving */

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load_str(h, "ssid", s_set.ssid, sizeof(s_set.ssid));
        load_str(h, "pass", s_set.pass, sizeof(s_set.pass));
        load_str(h, "tok",  s_set.token, sizeof(s_set.token));
        uint8_t u8;
        if (nvs_get_u8(h, "mode", &u8) == ESP_OK && u8 <= SCR_MODE_SPORT) {
            s_set.mode = u8;
        }
        if (nvs_get_u8(h, "dz", &u8) == ESP_OK && u8 <= 40) {
            s_set.deadzone_pct = u8;
        }
        nvs_close(h);
    }
}

void scr_settings_get(scr_settings_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_set;
    xSemaphoreGive(s_mtx);
}

void scr_settings_update(const scr_settings_t *in)
{
    if (in == NULL) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_set = *in;
    xSemaphoreGive(s_mtx);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed, settings not persisted");
        return;
    }
    nvs_set_str(h, "ssid", s_set.ssid);
    nvs_set_str(h, "pass", s_set.pass);
    nvs_set_str(h, "tok",  s_set.token);
    nvs_set_u8(h, "mode", s_set.mode);
    nvs_set_u8(h, "dz", s_set.deadzone_pct);
    if (nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs commit failed");
    }
    nvs_close(h);
}

void scr_settings_set_token(const char *token)
{
    scr_settings_t s;
    scr_settings_get(&s);
    strlcpy(s.token, token ? token : "", sizeof(s.token));
    scr_settings_update(&s);
}

void scr_settings_set_control(uint8_t mode, uint8_t deadzone_pct)
{
    scr_settings_t s;
    scr_settings_get(&s);
    s.mode = mode;
    s.deadzone_pct = deadzone_pct;
    scr_settings_update(&s);
}
