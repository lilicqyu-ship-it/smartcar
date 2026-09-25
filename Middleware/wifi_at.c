/* WiFi link driver for the ESP32-C6 AT module - runs entirely on CPU2
 * (bare-metal superloop). The module is connected to ASCLIN1 and its
 * interrupts are routed to CPU2. Decoded protocol frames are pushed to the
 * CPU0 control task through the xcore queue, status replies are built from
 * the status block CPU0 publishes, and debug output goes through the xcore
 * log bridge (CPU0 owns the console UART). */
#include "wifi_at.h"
#include "stime.h"
#include "xcore.h"
#include "protocol.h"
#include "robot.h"
#include "IfxAsclin_Asc.h"
#include "IfxAsclin_PinMap.h"
#include "IfxPort.h"

#include <string.h>

#define WIFI_BAUDRATE       115200

/* Interrupt priorities on CPU2 (no other sources on this core) */
#define WIFI_TX_PRIO        7
#define WIFI_RX_PRIO        5
#define WIFI_ER_PRIO        13

/* All three cores share one interrupt vector table (Lcf_*.lsl: __INTTAB_CPUn
 * are the same address), and this lsl only collects vector-table-0 entries.
 * Core routing is decided by the SRC TOS bit (IfxSrc_Tos_cpu2), so the ISRs
 * below MUST be declared on table 0, not table 2, or the linker discards
 * their entries and the ISRs never run. Priorities do not clash with CPU0
 * (1/2/4/8/12). */
#define WIFI_VECTAB         0

#define WIFI_TX_BUFFER_SIZE 512
#define WIFI_RX_BUFFER_SIZE 1024
#define WIFI_LINE_MAX       256
#define WIFI_IPD_BUFFER_SIZE 512
#define WIFI_HTTP_RESPONSE_MAX 1536

static IfxAsclin_Asc g_wifiAsclin;
static uint8 g_wifiTxBuffer[WIFI_TX_BUFFER_SIZE + sizeof(Ifx_Fifo) + 8];
static uint8 g_wifiRxBuffer[WIFI_RX_BUFFER_SIZE + sizeof(Ifx_Fifo) + 8];

static boolean g_clientConnected = FALSE;

IFX_INTERRUPT(wifiTxISR, WIFI_VECTAB, WIFI_TX_PRIO);

void wifiTxISR(void)
{
    IfxAsclin_Asc_isrTransmit(&g_wifiAsclin);
}

IFX_INTERRUPT(wifiRxISR, WIFI_VECTAB, WIFI_RX_PRIO);

void wifiRxISR(void)
{
    IfxAsclin_Asc_isrReceive(&g_wifiAsclin);
}

IFX_INTERRUPT(wifiErISR, WIFI_VECTAB, WIFI_ER_PRIO);

void wifiErISR(void)
{
    IfxAsclin_Asc_isrError(&g_wifiAsclin);
}

static boolean wifiReadByte(uint8 *byte, uint32 timeoutMs)
{
    uint32 start = STIME_nowMs();

    while ((STIME_nowMs() - start) < timeoutMs)
    {
        Ifx_SizeT count = 1;

        if (IfxAsclin_Asc_read(&g_wifiAsclin, byte, &count, TIME_NULL) && count == 1)
        {
            return TRUE;
        }
        STIME_delayMs(1);
    }
    return FALSE;
}

static void wifiSendBytes(const uint8 *data, uint32 len)
{
    Ifx_SizeT l = (Ifx_SizeT)len;

    IfxAsclin_Asc_write(&g_wifiAsclin, (void *)data, &l, TIME_INFINITE);
}

static void wifiSendString(const char *str)
{
    wifiSendBytes((const uint8 *)str, (uint32)strlen(str));
}

static void wifiAppendUint(char *buf, uint32 value)
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

static void wifiAppendInt(char *buf, sint32 value)
{
    if (value < 0)
    {
        strcat(buf, "-");
        wifiAppendUint(buf, (uint32)(-value));
    }
    else
    {
        wifiAppendUint(buf, (uint32)value);
    }
}

/* Queue one protocol command to the CPU0 control task */
static boolean wifiQueueCmd(uint8 cmd, const uint8 *data, uint32 len)
{
    XcoreCmdMsg msg;

    msg.cmd = cmd;
    msg.len = (uint8)len;
    if (len > 0 && data != NULL)
    {
        memcpy(msg.data, data, len);
    }
    return XCORE_cmdPush(&msg);
}

static void wifiQueueSpeedAndMotion(sint8 speed, uint8 motionCmd)
{
    wifiQueueCmd(PROTO_CMD_SET_SPEED, (const uint8 *)&speed, 1);
    wifiQueueCmd(motionCmd, NULL, 0);
}

static sint8 wifiHttpSpeed(const char *request)
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

static void wifiHttpSend(const char *contentType, const char *body)
{
    char response[WIFI_HTTP_RESPONSE_MAX];

    strcpy(response, "HTTP/1.1 200 OK\r\n");
    strcat(response, "Content-Type: ");
    strcat(response, contentType);
    strcat(response, "\r\nContent-Length: ");
    wifiAppendUint(response, (uint32)strlen(body));
    strcat(response, "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
    strcat(response, body);
    WIFI_sendRaw((const uint8 *)response, (uint32)strlen(response));
    WIFI_sendAtCommand("AT+CIPCLOSE=0", "OK", 1000);
}

static void wifiHttpHandleRequest(const char *request)
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
    ProtocolStatus status;

    if (strncmp(request, "GET /api/", 9) == 0)
    {
        const char *path = request + 9;
        sint8 speed = wifiHttpSpeed(request);

        wifiQueueCmd(PROTO_CMD_HEARTBEAT, NULL, 0);
        if (strncmp(path, "stop", 4) == 0)
        {
            wifiQueueCmd(PROTO_CMD_STOP, NULL, 0);
        }
        else if (strncmp(path, "forward", 7) == 0)
        {
            wifiQueueSpeedAndMotion(speed, PROTO_CMD_FORWARD);
        }
        else if (strncmp(path, "backward", 8) == 0)
        {
            wifiQueueSpeedAndMotion(speed, PROTO_CMD_BACKWARD);
        }
        else if (strncmp(path, "left", 4) == 0)
        {
            wifiQueueSpeedAndMotion(speed, PROTO_CMD_LEFT);
        }
        else if (strncmp(path, "right", 5) == 0)
        {
            wifiQueueSpeedAndMotion(speed, PROTO_CMD_RIGHT);
        }
        else if (strncmp(path, "heartbeat", 9) != 0 && strncmp(path, "status", 6) != 0)
        {
            wifiQueueCmd(PROTO_CMD_STOP, NULL, 0);
        }

        XCORE_statusGet(&status);
        strcpy(body, "{\"state\":");
        wifiAppendUint(body, status.state);
        strcat(body, ",\"left\":");
        wifiAppendInt(body, status.leftSpeed);
        strcat(body, ",\"right\":");
        wifiAppendInt(body, status.rightSpeed);
        strcat(body, ",\"heartbeat\":");
        wifiAppendUint(body, status.heartbeatOk ? 1U : 0U);
        strcat(body, "}");
        wifiHttpSend("application/json", body);
    }
    else
    {
        wifiHttpSend("text/html; charset=utf-8", page);
    }
}

boolean WIFI_waitForLine(char *line, uint32 lineSize, uint32 timeoutMs)
{
    uint32 idx = 0;
    uint32 start = STIME_nowMs();

    while ((STIME_nowMs() - start) < timeoutMs)
    {
        uint8 c;

        if (wifiReadByte(&c, 10))
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

boolean WIFI_sendAtCommand(const char *cmd, const char *okToken, uint32 timeoutMs)
{
    char line[WIFI_LINE_MAX];
    uint32 start = STIME_nowMs();

    XCORE_log("ESP-C6<- ");
    XCORE_logln(cmd);

    wifiSendString(cmd);
    wifiSendString("\r\n");

    while ((STIME_nowMs() - start) < timeoutMs)
    {
        if (!WIFI_waitForLine(line, sizeof(line), 200))
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
    XCORE_logln("WIFI: AT TIMEOUT");
    return FALSE;
}

static boolean wifiWaitForChar(char ch, uint32 timeoutMs)
{
    uint8 byte;
    uint32 start = STIME_nowMs();

    while ((STIME_nowMs() - start) < timeoutMs)
    {
        if (wifiReadByte(&byte, 10))
        {
            if (byte == (uint8)ch)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

boolean WIFI_sendRaw(const uint8 *data, uint32 len)
{
    char cmd[WIFI_LINE_MAX];
    char line[WIFI_LINE_MAX];
    boolean ok = FALSE;

    if (!g_clientConnected)
    {
        return FALSE;
    }

    /* Build: AT+CIPSEND=0,<len> */
    strcpy(cmd, "AT+CIPSEND=0,");
    wifiAppendUint(cmd, len);

    wifiSendString(cmd);
    wifiSendString("\r\n");

    /* Wait for the '>' data prompt (no newline) */
    if (!wifiWaitForChar('>', 2000))
    {
        XCORE_logln("WIFI: CIPSEND prompt failed");
        return FALSE;
    }

    wifiSendBytes(data, len);

    /* Wait for SEND OK */
    {
        uint32 start = STIME_nowMs();

        while ((STIME_nowMs() - start) < 2000)
        {
            if (WIFI_waitForLine(line, sizeof(line), 200))
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

    return ok;
}

void WIFI_init(void)
{
    IfxAsclin_Asc_Config ascConf;

    IfxAsclin_Asc_initModuleConfig(&ascConf, &MODULE_ASCLIN1);

    ascConf.baudrate.baudrate       = WIFI_BAUDRATE;
    ascConf.baudrate.oversampling   = IfxAsclin_OversamplingFactor_16;
    ascConf.bitTiming.medianFilter  = IfxAsclin_SamplesPerBit_three;
    ascConf.bitTiming.samplePointPosition = IfxAsclin_SamplePointPosition_8;

    ascConf.interrupt.txPriority    = WIFI_TX_PRIO;
    ascConf.interrupt.rxPriority    = WIFI_RX_PRIO;
    ascConf.interrupt.erPriority    = WIFI_ER_PRIO;
    ascConf.interrupt.typeOfService = IfxSrc_Tos_cpu2;

    const IfxAsclin_Asc_Pins pins = {
            NULL_PTR,                       IfxPort_InputMode_pullUp,
            &IfxAsclin1_RXA_P15_1_IN,       IfxPort_InputMode_pullUp,
            NULL_PTR,                       IfxPort_OutputMode_pushPull,
            &IfxAsclin1_TX_P15_0_OUT,       IfxPort_OutputMode_pushPull,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
    };
    ascConf.pins = &pins;

    ascConf.txBuffer     = g_wifiTxBuffer;
    ascConf.txBufferSize = WIFI_TX_BUFFER_SIZE;
    ascConf.rxBuffer     = g_wifiRxBuffer;
    ascConf.rxBufferSize = WIFI_RX_BUFFER_SIZE;

    IfxAsclin_Asc_initModule(&g_wifiAsclin, &ascConf);
}

typedef enum
{
    WIFI_MSG_NONE = 0,
    WIFI_MSG_IPD,           /* binary data frame, payload filled */
    WIFI_MSG_LINE           /* status line (CONNECT/CLOSED/...), line filled */
} WifiMsgType;

static WifiMsgType wifiReadMessage(uint8 *payload, uint32 maxPayload, uint32 *payloadLen,
                                   char *line, uint32 lineSize)
{
    /* Byte-level reader. A line is accumulated up to '\n' and returned as
     * WIFI_MSG_LINE. Only lines starting with "+IPD," are treated as data
     * frames: their header ends at ':' and exactly <len> payload bytes follow
     * (binary safe). esp-at v3 also emits event lines with colons, e.g.
     * +STA_CONNECTED:"192.168.4.2" - the colon there is an ordinary character,
     * otherwise its tail would leak into the stream as a garbage line. */
    char    header[64];
    uint32  hIdx = 0;
    uint8   byte;
    uint32  linkId = 0;
    uint32  len    = 0;
    boolean gotColon = FALSE;

    *payloadLen = 0;
    line[0] = '\0';

    /* Accumulate until end of line, or until the ':' that terminates an IPD header */
    while (1)
    {
        if (!wifiReadByte(&byte, 500))
        {
            return WIFI_MSG_NONE;
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
            return WIFI_MSG_LINE;
        }
        if ((byte == ':') && (hIdx >= 5) && (strncmp(header, "+IPD,", 5) == 0))
        {
            header[hIdx] = '\0';
            gotColon = TRUE;
            break;
        }
        if (hIdx < sizeof(header) - 1)
        {
            header[hIdx++] = (char)byte;
        }
        /* overlong lines keep consuming until '\n' (returned truncated) */
    }
    if (!gotColon)
    {
        return WIFI_MSG_NONE;
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
            return WIFI_MSG_NONE;
        }
        p++;
        while (*p >= '0' && *p <= '9')
        {
            len = len * 10 + (uint32)(*p - '0');
            p++;
        }
        if (len == 0 || len > maxPayload)
        {
            return WIFI_MSG_NONE;
        }
    }

    /* Read exactly len payload bytes */
    {
        uint32 i;

        for (i = 0; i < len; i++)
        {
            if (!wifiReadByte(&byte, 500))
            {
                return WIFI_MSG_NONE;
            }
            payload[i] = byte;
        }
    }

    /* Drain trailing CR LF (and anything left over) */
    {
        uint32 i;

        for (i = 0; i < 8; i++)
        {
            if (!wifiReadByte(&byte, 100))
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
    return WIFI_MSG_IPD;
}

void WIFI_main(void)
{
    char line[WIFI_LINE_MAX];
    uint32 attempts;

    STIME_delayMs(3000);                 /* let the AT firmware finish booting */

    XCORE_logln("WIFI: starting ESP32-C6 AT...");

    while (WIFI_waitForLine(line, sizeof(line), 200))
    {
        XCORE_log("ESP-C6<- ");
        XCORE_logln(line);
    }

    XCORE_logln("WIFI: ATE0 (echo off)");
    for (attempts = 0; attempts < 5; attempts++)
    {
        if (WIFI_sendAtCommand("ATE0", "OK", 2000))
        {
            break;
        }
        STIME_delayMs(1000);
    }

    XCORE_logln("WIFI: AP mode (CWMODE=2)");
    WIFI_sendAtCommand("AT+CWMODE=2", "OK", 2000);

    XCORE_logln("WIFI: set AP credentials");
    {
        char cmd[WIFI_LINE_MAX];

        strcpy(cmd, "AT+CWSAP=\"");
        strcat(cmd, WIFI_SSID);
        strcat(cmd, "\",\"");
        strcat(cmd, WIFI_PASS);
        strcat(cmd, "\",11,3");
        WIFI_sendAtCommand(cmd, "OK", 3000);
    }

    XCORE_logln("WIFI: enable multiple connections");
    WIFI_sendAtCommand("AT+CIPMUX=1", "OK", 2000);

    XCORE_logln("WIFI: start TCP server");
    {
        char cmd[WIFI_LINE_MAX];

        strcpy(cmd, "AT+CIPSERVER=1,");
        wifiAppendUint(cmd, WIFI_TCP_PORT);
        WIFI_sendAtCommand(cmd, "OK", 2000);
    }

    XCORE_logln("WIFI: AP ready, waiting for phone...");

    while (1)
    {
        /* Supports both binary protocol frames and HTTP requests. */
        uint8 frame[WIFI_IPD_BUFFER_SIZE];
        uint32 frameLen;
        char statusLine[WIFI_LINE_MAX];
        WifiMsgType msg;

        msg = wifiReadMessage(frame, sizeof(frame), &frameLen, statusLine, sizeof(statusLine));

        if (msg == WIFI_MSG_IPD)
        {
            g_clientConnected = TRUE;
            if (frameLen >= 5 && memcmp(frame, "GET ", 4) == 0)
            {
                frame[frameLen < sizeof(frame) ? frameLen : sizeof(frame) - 1] = '\0';
                XCORE_logln("WIFI: HTTP request");
                wifiHttpHandleRequest((const char *)frame);
            }
            else
            {
                XCORE_log("WIFI: RX frame, ");
                wifiAppendUint(statusLine, frameLen);
                XCORE_logln(statusLine);
                {
                    uint32 i;

                    for (i = 0; i < frameLen; i++)
                    {
                        PROTO_feedByte(frame[i]);
                    }
                }
            }
        }
        else if (msg == WIFI_MSG_LINE)
        {
            if (statusLine[0] != '\0')   /* skip empty CR LF keep-alive lines */
            {
                XCORE_log("ESP-C6<- ");
                XCORE_logln(statusLine);
            }
            if (strstr(statusLine, "CONNECT FAIL"))
            {
                g_clientConnected = FALSE;
                XCORE_logln("WIFI: TCP connect failed");
            }
            else if (strstr(statusLine, "CONNECT"))
            {
                g_clientConnected = TRUE;
                XCORE_logln("WIFI: phone connected");
            }
            else if (strstr(statusLine, "CLOSED"))
            {
                g_clientConnected = FALSE;
                XCORE_logln("WIFI: phone disconnected");
            }
        }
        else
        {
            STIME_delayMs(2);            /* idle: nothing from the module */
        }
    }
}
