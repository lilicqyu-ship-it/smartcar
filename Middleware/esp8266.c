#include "esp8266.h"
#include "uart.h"
#include "protocol.h"
#include "robot.h"
#include "IfxAsclin_Asc.h"
#include "IfxAsclin_PinMap.h"
#include "IfxPort.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define ESP_BAUDRATE        115200

#define ESP_TX_PRIO         7
#define ESP_RX_PRIO         5
#define ESP_ER_PRIO         13

#define ESP_TX_BUFFER_SIZE  512
#define ESP_RX_BUFFER_SIZE  1024
#define ESP_LINE_MAX        256
#define ESP_IPD_BUFFER_SIZE 512
#define ESP_HTTP_RESPONSE_MAX 1536

static IfxAsclin_Asc g_espAsclin;
static uint8 g_espTxBuffer[ESP_TX_BUFFER_SIZE + sizeof(Ifx_Fifo) + 8];
static uint8 g_espRxBuffer[ESP_RX_BUFFER_SIZE + sizeof(Ifx_Fifo) + 8];

static volatile boolean g_clientConnected = FALSE;
static SemaphoreHandle_t g_espTxMutex = NULL;

IFX_INTERRUPT(espTxISR, 0, ESP_TX_PRIO);

void espTxISR(void)
{
    IfxAsclin_Asc_isrTransmit(&g_espAsclin);
}

IFX_INTERRUPT(espRxISR, 0, ESP_RX_PRIO);

void espRxISR(void)
{
    IfxAsclin_Asc_isrReceive(&g_espAsclin);
}

IFX_INTERRUPT(espErISR, 0, ESP_ER_PRIO);

void espErISR(void)
{
    IfxAsclin_Asc_isrError(&g_espAsclin);
}

static boolean espReadByte(uint8 *byte, uint32 timeoutMs)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeoutMs);

    while ((xTaskGetTickCount() - start) < timeout)
    {
        Ifx_SizeT count = 1;

        if (IfxAsclin_Asc_read(&g_espAsclin, byte, &count, TIME_NULL) && count == 1)
        {
            return TRUE;
        }
        vTaskDelay(1);
    }
    return FALSE;
}

static void espSendBytes(const uint8 *data, uint32 len)
{
    Ifx_SizeT l = (Ifx_SizeT)len;

    IfxAsclin_Asc_write(&g_espAsclin, (void *)data, &l, TIME_INFINITE);
}

static void espSendString(const char *str)
{
    espSendBytes((const uint8 *)str, (uint32)strlen(str));
}

static void espAppendUint(char *buf, uint32 value)
{
    char tmp[12];
    uint32 i = 0;
    uint32 j;

    if (value == 0)
    {
        tmp[i++] = '0';
    }
    else
    {
        while (value > 0)
        {
            tmp[i++] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    j = strlen(buf);
    while (i > 0)
    {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

static void espAppendInt(char *buf, sint32 value)
{
    if (value < 0)
    {
        strcat(buf, "-");
        espAppendUint(buf, (uint32)(-value));
    }
    else
    {
        espAppendUint(buf, (uint32)value);
    }
}

static sint8 espHttpSpeed(const char *request)
{
    const char *p = strstr(request, "speed=");
    sint32 value = ROBOT_DEFAULT_SPEED;

    if (p != NULL)
    {
        p += 6;
        value = 0;
        while (*p >= '0' && *p <= '9')
        {
            value = value * 10 + (*p - '0');
            p++;
        }
    }
    if (value < 0)
    {
        value = 0;
    }
    if (value > 100)
    {
        value = 100;
    }
    return (sint8)value;
}

static void espHttpSend(const char *contentType, const char *body)
{
    char response[ESP_HTTP_RESPONSE_MAX];

    strcpy(response, "HTTP/1.1 200 OK\r\n");
    strcat(response, "Content-Type: ");
    strcat(response, contentType);
    strcat(response, "\r\nContent-Length: ");
    espAppendUint(response, (uint32)strlen(body));
    strcat(response, "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
    strcat(response, body);
    ESP8266_sendRaw((const uint8 *)response, (uint32)strlen(response));
    ESP8266_sendAtCommand("AT+CIPCLOSE=0", "OK", 1000);
}

static void espHttpHandleRequest(const char *request)
{
    static const char page[] =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>AURIX SmartDrive</title><style>body{text-align:center;font:20px sans-serif}"
        "button{width:110px;height:60px;margin:6px;font-size:20px}#s{width:260px}</style>"
        "<h2>AURIX SmartDrive</h2><button onpointerdown=go('forward') onpointerup=stop>UP</button><br>"
        "<button onpointerdown=go('left') onpointerup=stop>LEFT</button>"
        "<button onclick=stop>STOP</button><button onpointerdown=go('right') onpointerup=stop>RIGHT</button><br>"
        "<button onpointerdown=go('backward') onpointerup=stop>DOWN</button><br>"
        "Speed <input id=s type=range min=0 max=100 value=50><pre id=x>OFFLINE</pre>"
        "<script>let t;function q(u){fetch('/api/'+u).then(r=>r.text()).then(x=>document.querySelector('#x').textContent=x)}"
        "function go(d){q(d+'?speed='+s.value);clearInterval(t);t=setInterval(()=>q('heartbeat'),50)}"
        "function stop(){clearInterval(t);q('stop')}</script>";
    char body[192];
    RobotSpeeds speeds;
    uint8 state;

    if (strncmp(request, "GET /api/", 9) == 0)
    {
        const char *path = request + 9;
        sint8 speed = espHttpSpeed(request);

        ROBOT_cmdHeartbeat();
        if (strncmp(path, "stop", 4) == 0)
        {
            ROBOT_cmdStop();
        }
        else if (strncmp(path, "forward", 7) == 0)
        {
            ROBOT_cmdSetSpeed(speed);
            ROBOT_cmdMotion(PROTO_CMD_FORWARD);
        }
        else if (strncmp(path, "backward", 8) == 0)
        {
            ROBOT_cmdSetSpeed(speed);
            ROBOT_cmdMotion(PROTO_CMD_BACKWARD);
        }
        else if (strncmp(path, "left", 4) == 0)
        {
            ROBOT_cmdSetSpeed(speed);
            ROBOT_cmdMotion(PROTO_CMD_LEFT);
        }
        else if (strncmp(path, "right", 5) == 0)
        {
            ROBOT_cmdSetSpeed(speed);
            ROBOT_cmdMotion(PROTO_CMD_RIGHT);
        }
        else if (strncmp(path, "heartbeat", 9) != 0 && strncmp(path, "status", 6) != 0)
        {
            ROBOT_cmdStop();
        }

        state = ROBOT_getState();
        speeds = ROBOT_getSpeeds();
        strcpy(body, "{\"state\":");
        espAppendUint(body, state);
        strcat(body, ",\"left\":");
        espAppendInt(body, speeds.left);
        strcat(body, ",\"right\":");
        espAppendInt(body, speeds.right);
        strcat(body, ",\"heartbeat\":");
        espAppendUint(body, ROBOT_isHeartbeatOk() ? 1U : 0U);
        strcat(body, "}");
        espHttpSend("application/json", body);
    }
    else
    {
        espHttpSend("text/html; charset=utf-8", page);
    }
}

boolean ESP8266_waitForLine(char *line, uint32 lineSize, uint32 timeoutMs)
{
    uint32 idx = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeoutMs);

    while ((xTaskGetTickCount() - start) < timeout)
    {
        uint8 c;

        if (espReadByte(&c, 10))
        {
            if (c == '\n')
            {
                if (idx > 0 && line[idx - 1] == '\r')
                {
                    idx--;
                }
                line[idx] = '\0';
                return TRUE;
            }
            if (idx < lineSize - 1)
            {
                line[idx++] = (char)c;
            }
        }
    }
    return FALSE;
}

boolean ESP8266_sendAtCommand(const char *cmd, const char *okToken, uint32 timeoutMs)
{
    char line[ESP_LINE_MAX];
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeoutMs);

    UART_print("ESP-> ");
    UART_println(cmd);

    espSendString(cmd);
    espSendString("\r\n");

    while ((xTaskGetTickCount() - start) < timeout)
    {
        if (!ESP8266_waitForLine(line, sizeof(line), 200))
        {
            continue;
        }
        if (strstr(line, "ERROR") || strstr(line, "FAIL"))
        {
            return FALSE;
        }
        if (strstr(line, okToken))
        {
            return TRUE;
        }
    }
    UART_println("ESP8266: AT TIMEOUT");
    return FALSE;
}

static boolean espWaitForChar(char ch, uint32 timeoutMs)
{
    uint8 byte;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeoutMs);

    while ((xTaskGetTickCount() - start) < timeout)
    {
        if (espReadByte(&byte, 10))
        {
            if (byte == (uint8)ch)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

boolean ESP8266_sendRaw(const uint8 *data, uint32 len)
{
    char cmd[ESP_LINE_MAX];
    char line[ESP_LINE_MAX];
    boolean ok = FALSE;

    if (g_espTxMutex != NULL)
    {
        if (xSemaphoreTake(g_espTxMutex, pdMS_TO_TICKS(2000)) != pdTRUE)
        {
            return FALSE;
        }
    }

    if (!g_clientConnected)
    {
        goto done;
    }

    /* Build: AT+CIPSEND=0,<len> */
    strcpy(cmd, "AT+CIPSEND=0,");
    espAppendUint(cmd, len);

    espSendString(cmd);
    espSendString("\r\n");

    /* Wait for the '>' data prompt (no newline) */
    if (!espWaitForChar('>', 2000))
    {
        UART_println("ESP8266: CIPSEND prompt failed");
        goto done;
    }

    espSendBytes(data, len);

    /* Wait for SEND OK */
    {
        TickType_t start = xTaskGetTickCount();
        TickType_t timeout = pdMS_TO_TICKS(2000);

        while ((xTaskGetTickCount() - start) < timeout)
        {
            if (ESP8266_waitForLine(line, sizeof(line), 200))
            {
                if (strstr(line, "SEND OK"))
                {
                    ok = TRUE;
                    break;
                }
                if (strstr(line, "ERROR") || strstr(line, "FAIL") || strstr(line, "CLOSED"))
                {
                    break;
                }
            }
        }
    }

done:
    if (g_espTxMutex != NULL)
    {
        xSemaphoreGive(g_espTxMutex);
    }
    return ok;
}

void ESP8266_init(void)
{
    IfxAsclin_Asc_Config ascConf;

    g_espTxMutex = xSemaphoreCreateMutex();

    IfxAsclin_Asc_initModuleConfig(&ascConf, &MODULE_ASCLIN1);

    ascConf.baudrate.baudrate       = ESP_BAUDRATE;
    ascConf.baudrate.oversampling   = IfxAsclin_OversamplingFactor_16;
    ascConf.bitTiming.medianFilter  = IfxAsclin_SamplesPerBit_three;
    ascConf.bitTiming.samplePointPosition = IfxAsclin_SamplePointPosition_8;

    ascConf.interrupt.txPriority    = ESP_TX_PRIO;
    ascConf.interrupt.rxPriority    = ESP_RX_PRIO;
    ascConf.interrupt.erPriority    = ESP_ER_PRIO;
    ascConf.interrupt.typeOfService = IfxSrc_Tos_cpu0;

    const IfxAsclin_Asc_Pins pins = {
            NULL_PTR,                       IfxPort_InputMode_pullUp,
            &IfxAsclin1_RXA_P15_1_IN,       IfxPort_InputMode_pullUp,
            NULL_PTR,                       IfxPort_OutputMode_pushPull,
            &IfxAsclin1_TX_P15_0_OUT,       IfxPort_OutputMode_pushPull,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
    };
    ascConf.pins = &pins;

    ascConf.txBuffer     = g_espTxBuffer;
    ascConf.txBufferSize = ESP_TX_BUFFER_SIZE;
    ascConf.rxBuffer     = g_espRxBuffer;
    ascConf.rxBufferSize = ESP_RX_BUFFER_SIZE;

    IfxAsclin_Asc_initModule(&g_espAsclin, &ascConf);
}

typedef enum
{
    ESP_MSG_NONE = 0,
    ESP_MSG_IPD,            /* binary data frame, payload filled */
    ESP_MSG_LINE            /* status line (CONNECT/CLOSED/...), line filled */
} EspMsgType;

static EspMsgType espReadMessage(uint8 *payload, uint32 maxPayload, uint32 *payloadLen,
                                 char *line, uint32 lineSize)
{
    /* Byte-level reader. Reads the ASCII header up to ':' (IPD frame) or '\n' (status line),
     * then for IPD reads exactly <len> payload bytes (binary safe). */
    char    header[64];
    uint32  hIdx = 0;
    uint8   byte;
    uint32  linkId = 0;
    uint32  len    = 0;
    boolean gotColon = FALSE;

    *payloadLen = 0;
    line[0] = '\0';

    /* Accumulate header until ':' or '\n' */
    while (hIdx < sizeof(header) - 1)
    {
        if (!espReadByte(&byte, 500))
        {
            return ESP_MSG_NONE;
        }
        if (byte == ':')
        {
            header[hIdx] = '\0';
            gotColon = TRUE;
            break;
        }
        if (byte == '\n')
        {
            if (hIdx > 0 && header[hIdx - 1] == '\r')
            {
                hIdx--;
            }
            header[hIdx] = '\0';
            if (hIdx < lineSize)
            {
                memcpy(line, header, hIdx);
            }
            line[lineSize - 1] = '\0';
            return ESP_MSG_LINE;
        }
        header[hIdx++] = (char)byte;
    }
    if (!gotColon)
    {
        return ESP_MSG_NONE;
    }

    if (strncmp(header, "+IPD,", 5) != 0)
    {
        return ESP_MSG_NONE;
    }

    /* Parse link id and length */
    {
        const char *p = header + 5;

        while (*p >= '0' && *p <= '9')
        {
            linkId = linkId * 10 + (uint32)(*p - '0');
            p++;
        }
        if (*p != ',')
        {
            return ESP_MSG_NONE;
        }
        p++;
        while (*p >= '0' && *p <= '9')
        {
            len = len * 10 + (uint32)(*p - '0');
            p++;
        }
        if (len == 0 || len > maxPayload)
        {
            return ESP_MSG_NONE;
        }
    }

    /* Read exactly len payload bytes */
    {
        uint32 i;

        for (i = 0; i < len; i++)
        {
            if (!espReadByte(&byte, 500))
            {
                return ESP_MSG_NONE;
            }
            payload[i] = byte;
        }
    }

    /* Drain trailing CR LF (and anything left over) */
    {
        uint32 i;

        for (i = 0; i < 8; i++)
        {
            if (!espReadByte(&byte, 100))
            {
                break;
            }
            if (byte == '\n')
            {
                break;
            }
        }
    }

    *payloadLen = len;
    (void)linkId;
    return ESP_MSG_IPD;
}

void ESP8266_task(void *pvParameters)
{
    char line[ESP_LINE_MAX];
    uint32 attempts;

    vTaskDelay(pdMS_TO_TICKS(3000));

    UART_println("ESP8266: starting...");

    while (ESP8266_waitForLine(line, sizeof(line), 200))
    {
        UART_print("ESP<- ");
        UART_println(line);
    }

    UART_println("ESP8266: ATE0 (echo off)");
    for (attempts = 0; attempts < 5; attempts++)
    {
        if (ESP8266_sendAtCommand("ATE0", "OK", 2000))
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    UART_println("ESP8266: AP mode (CWMODE=2)");
    ESP8266_sendAtCommand("AT+CWMODE=2", "OK", 2000);

    UART_println("ESP8266: set AP credentials");
    {
        char cmd[ESP_LINE_MAX];

        strcpy(cmd, "AT+CWSAP=\"");
        strcat(cmd, ESP8266_WIFI_SSID);
        strcat(cmd, "\",\"");
        strcat(cmd, ESP8266_WIFI_PASS);
        strcat(cmd, "\",11,3");
        ESP8266_sendAtCommand(cmd, "OK", 3000);
    }

    UART_println("ESP8266: enable multiple connections");
    ESP8266_sendAtCommand("AT+CIPMUX=1", "OK", 2000);

    UART_println("ESP8266: start TCP server");
    {
        char cmd[ESP_LINE_MAX];

        strcpy(cmd, "AT+CIPSERVER=1,");
        espAppendUint(cmd, ESP8266_TCP_PORT);
        ESP8266_sendAtCommand(cmd, "OK", 2000);
    }

    UART_println("ESP8266: AP ready, waiting for phone...");

    while (1)
    {
        /* Supports both binary protocol frames and HTTP requests. */
        uint8 frame[ESP_IPD_BUFFER_SIZE];
        uint32 frameLen;
        char statusLine[ESP_LINE_MAX];
        EspMsgType msg;

        msg = ESP_MSG_NONE;
        if (g_espTxMutex != NULL)
        {
            if (xSemaphoreTake(g_espTxMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
            {
                msg = espReadMessage(frame, sizeof(frame), &frameLen, statusLine, sizeof(statusLine));
                xSemaphoreGive(g_espTxMutex);
            }
        }
        else
        {
            msg = espReadMessage(frame, sizeof(frame), &frameLen, statusLine, sizeof(statusLine));
        }

        if (msg == ESP_MSG_IPD)
        {
            uint32 i;

            g_clientConnected = TRUE;
            if (frameLen >= 5 && memcmp(frame, "GET ", 4) == 0)
            {
                frame[frameLen < sizeof(frame) ? frameLen : sizeof(frame) - 1] = '\0';
                UART_println("ESP8266: HTTP request");
                espHttpHandleRequest((const char *)frame);
            }
            else
            {
                UART_print("ESP8266: RX frame, ");
                espAppendUint(statusLine, frameLen);
                UART_println(statusLine);
                for (i = 0; i < frameLen; i++)
                {
                    PROTO_feedByte(frame[i]);
                }
                PROTO_process();
            }
        }
        else if (msg == ESP_MSG_LINE)
        {
            UART_print("ESP<- ");
            UART_println(statusLine);
            if (strstr(statusLine, "CONNECT FAIL"))
            {
                g_clientConnected = FALSE;
                UART_println("ESP8266: TCP connect failed");
            }
            else if (strstr(statusLine, "CONNECT"))
            {
                g_clientConnected = TRUE;
                UART_println("ESP8266: phone connected");
            }
            else if (strstr(statusLine, "CLOSED"))
            {
                g_clientConnected = FALSE;
                UART_println("ESP8266: phone disconnected");
            }
        }
    }
}
