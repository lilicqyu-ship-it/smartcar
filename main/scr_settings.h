/*
 * scr_settings.h - NVS-backed user settings of the remote controller.
 *
 * A single struct, loaded at boot, written back on change.  The Wi-Fi
 * credentials and the session token bridge the S3 to the C6 softAP the same
 * way the phone web page does (esp32c6_car doc 05/06).
 */
#ifndef SCR_SETTINGS_H
#define SCR_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCR_SSID_MAX   33     /* 32 SSID bytes + NUL  */
#define SCR_PASS_MAX   65     /* 64 PSK bytes + NUL   */
#define SCR_TOKEN_MAX  65     /* 64 hex chars + NUL   */

typedef struct {
    char    ssid[SCR_SSID_MAX];
    char    pass[SCR_PASS_MAX];
    char    token[SCR_TOKEN_MAX];   /* pairing token, empty = unpaired  */
    uint8_t mode;                   /* scr_mode_t                       */
    uint8_t deadzone_pct;           /* radial dead zone, % of radius    */
} scr_settings_t;

void scr_settings_init(void);

/* Read a consistent copy. */
void scr_settings_get(scr_settings_t *out);

/* Replace everything and persist to NVS (best effort, logged on failure). */
void scr_settings_update(const scr_settings_t *in);

/* Convenience: persist just the token after pairing. */
void scr_settings_set_token(const char *token);

/* Convenience: persist mode / deadzone from the Control settings page. */
void scr_settings_set_control(uint8_t mode, uint8_t deadzone_pct);

#ifdef __cplusplus
}
#endif

#endif /* SCR_SETTINGS_H */
