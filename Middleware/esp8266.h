#ifndef ESP8266_H
#define ESP8266_H

#include "Ifx_Types.h"

/* AP mode credentials (requirement.md section 5.1) */
#define ESP8266_WIFI_SSID   "AURIX-SmartDrive"
#define ESP8266_WIFI_PASS   "12345678"
#define ESP8266_TCP_PORT    8080  /* HTTP server: http://192.168.4.1:8080 */

void ESP8266_init(void);
boolean ESP8266_sendAtCommand(const char *cmd, const char *okToken, uint32 timeoutMs);
boolean ESP8266_waitForLine(char *line, uint32 lineSize, uint32 timeoutMs);
boolean ESP8266_sendRaw(const uint8 *data, uint32 len);
void ESP8266_task(void *pvParameters);

#endif
