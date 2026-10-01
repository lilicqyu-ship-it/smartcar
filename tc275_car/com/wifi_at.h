#ifndef WIFI_AT_H
#define WIFI_AT_H

#include "Ifx_Types.h"

/* WiFi module driver for an ESP32-C6 running the ESP-AT firmware
 * (source of the flashed firmware: ../esp-at, module esp32c6_default).
 * Runs on CPU2 (bare-metal); the module hangs off ASCLIN1 (P15.0 TX / P15.1 RX).
 * ESP-AT v3 keeps the classic command set used here:
 *   AT+CWMODE=2 / AT+CWSAP (softAP), AT+CIPMUX=1, AT+CIPSERVER=1,<port>,
 *   AT+CIPSEND=<link>,<len> ('>' prompt, then "SEND OK"),
 *   +IPD,<link>,<len>:<data> (AT+CIPDINFO=0, the default),
 *   "0,CONNECT" / "0,CLOSED" connection events. */

/* softAP credentials (requirement.md section 5.1) */
#define WIFI_SSID      "AURIX-SmartDrive"
#define WIFI_PASS      "12345678"
#define WIFI_TCP_PORT  8080  /* HTTP server: http://192.168.4.1:8080 */

void WIFI_init(void);
void WIFI_main(void);                     /* never returns - CPU2 superloop */

boolean WIFI_sendAtCommand(const char *cmd, const char *okToken, uint32 timeoutMs);
boolean WIFI_waitForLine(char *line, uint32 lineSize, uint32 timeoutMs);
boolean WIFI_sendRaw(const uint8 *data, uint32 len);

#endif
