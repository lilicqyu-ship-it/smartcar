/* Binary control protocol, split across two cores:
 *  - CPU2 (WiFi owner) parses the byte stream (PROTO_feedByte). A valid frame
 *    is routed in PROTO_routeFrame: GET_STATUS is answered right away from the
 *    status block CPU0 publishes, EMERGENCY_STOP additionally latches the
 *    fast-path e-stop bypass, everything else is queued to CPU0.
 *  - CPU0 (robot state owner) executes commands in PROTO_handleCommand,
 *    called from the 10 ms control task with the xcore queue drained. */
#include "mw/proto/protocol.h"
#include "app/robot.h"
#include "mw/xcore/xcore.h"
#include "com/wifi_at.h"

#include <string.h>

#define PROTO_CRC_SEED 0x00

typedef enum
{
    STATE_HEADER1 = 0,
    STATE_HEADER2,
    STATE_CMD,
    STATE_LEN,
    STATE_DATA,
    STATE_CRC
} ParseState;

typedef struct
{
    ParseState state;
    uint8      cmd;
    uint8      len;
    uint8      payload[PROTO_MAX_PAYLOAD];
    uint8      idx;
    uint8      crc;
} ProtoParser;

static ProtoParser g_parser;
static ProtoResult g_lastResult;

static uint8 PROTO_calcCrc(uint8 cmd, const uint8 *data, uint8 len)
{
    uint8 crc = PROTO_CRC_SEED;
    uint8 i;

    crc ^= cmd;
    crc ^= len;
    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
    }
    return crc;
}

/* CPU0 only: execute a validated command against the robot controller */
void PROTO_handleCommand(uint8 cmd, const uint8 *data, uint8 len)
{
    switch (cmd)
    {
    case PROTO_CMD_STOP:
        ROBOT_cmdStop();
        break;

    case PROTO_CMD_FORWARD:
    case PROTO_CMD_BACKWARD:
    case PROTO_CMD_LEFT:
    case PROTO_CMD_RIGHT:
    case PROTO_CMD_FORWARD_LEFT:
    case PROTO_CMD_FORWARD_RIGHT:
    case PROTO_CMD_ROTATE_LEFT:
    case PROTO_CMD_ROTATE_RIGHT:
        ROBOT_cmdMotion(cmd);
        break;

    case PROTO_CMD_SET_SPEED:
        if (len >= 2)
        {
            ROBOT_cmdSetSpeeds((sint8)data[0], (sint8)data[1]);
        }
        else if (len >= 1)
        {
            ROBOT_cmdSetSpeed((sint8)data[0]);
        }
        break;

    /* GET_STATUS is answered by CPU2 from the published status block */

    case PROTO_CMD_HEARTBEAT:
        ROBOT_cmdHeartbeat();
        break;

    case PROTO_CMD_RESET:
        ROBOT_cmdReset();
        break;

    case PROTO_CMD_CLEAR_FAULT:
        ROBOT_cmdClearFault();
        break;

    case PROTO_CMD_EMERGENCY_STOP:
        ROBOT_cmdEmergencyStop();
        break;

    default:
        break;
    }
}

/* CPU2 only: route a CRC-checked frame */
static void PROTO_routeFrame(uint8 cmd, const uint8 *data, uint8 len)
{
    XcoreCmdMsg msg;

    if (cmd == PROTO_CMD_GET_STATUS)
    {
        ProtocolStatus status;

        XCORE_statusGet(&status);
        PROTO_sendStatus(&status);
        return;
    }

    /* Queue first, then latch the bypass: once CPU0 has seen the command the
     * fault is latched and it will not clear the bypass; if CPU0 drains before
     * the command lands, the bypass set afterwards still stands. */
    msg.cmd = cmd;
    msg.len = len;
    memcpy(msg.data, data, len);
    (void)XCORE_cmdPush(&msg);

    if (cmd == PROTO_CMD_EMERGENCY_STOP)
    {
        /* Fast path: brake on CPU1 without waiting for the CPU0 control task */
        XCORE_estopRequest();
    }
}

void PROTO_init(void)
{
    memset(&g_parser, 0, sizeof(g_parser));
    g_parser.state = STATE_HEADER1;
    g_lastResult   = PROTO_RESULT_IDLE;
}

void PROTO_feedByte(uint8 byte)
{
    switch (g_parser.state)
    {
    case STATE_HEADER1:
        g_parser.crc = PROTO_CRC_SEED;
        if (byte == PROTO_HEADER1)
        {
            g_parser.state = STATE_HEADER2;
        }
        break;

    case STATE_HEADER2:
        if (byte == PROTO_HEADER2)
        {
            g_parser.state = STATE_CMD;
        }
        else
        {
            g_parser.state = (byte == PROTO_HEADER1) ? STATE_HEADER2 : STATE_HEADER1;
        }
        break;

    case STATE_CMD:
        g_parser.cmd   = byte;
        g_parser.state = STATE_LEN;
        break;

    case STATE_LEN:
        g_parser.len = byte;
        g_parser.idx = 0;
        if (g_parser.len > PROTO_MAX_PAYLOAD)
        {
            g_parser.state = STATE_HEADER1;
            g_lastResult   = PROTO_RESULT_ERROR;
        }
        else
        {
            g_parser.state = (g_parser.len > 0) ? STATE_DATA : STATE_CRC;
        }
        break;

    case STATE_DATA:
        g_parser.payload[g_parser.idx++] = byte;
        if (g_parser.idx >= g_parser.len)
        {
            g_parser.state = STATE_CRC;
        }
        break;

    case STATE_CRC:
        if (byte == PROTO_calcCrc(g_parser.cmd, g_parser.payload, g_parser.len))
        {
            PROTO_routeFrame(g_parser.cmd, g_parser.payload, g_parser.len);
            g_lastResult = PROTO_RESULT_FRAME;
        }
        else
        {
            g_lastResult = PROTO_RESULT_ERROR;
        }
        g_parser.state = STATE_HEADER1;
        break;

    default:
        g_parser.state = STATE_HEADER1;
        break;
    }
}

ProtoResult PROTO_process(void)
{
    return g_lastResult;
}

boolean PROTO_sendBytes(const uint8 *data, uint32 len)
{
    return WIFI_sendRaw(data, len);
}

void PROTO_sendStatus(const ProtocolStatus *status)
{
    uint8 frame[4 + 6 + 1];
    uint8 data[6];
    uint8 crc;
    uint8 i;

    data[0] = status->state;
    data[1] = (uint8)status->leftSpeed;
    data[2] = (uint8)status->rightSpeed;
    data[3] = status->heartbeatOk;
    data[4] = status->faultCode;
    data[5] = status->emergencyStop;

    crc = PROTO_calcCrc(PROTO_CMD_STATUS_REPLY, data, 6);

    frame[0] = PROTO_HEADER1;
    frame[1] = PROTO_HEADER2;
    frame[2] = PROTO_CMD_STATUS_REPLY;
    frame[3] = 6;
    for (i = 0; i < 6; i++)
    {
        frame[4 + i] = data[i];
    }
    frame[10] = crc;

    PROTO_sendBytes(frame, sizeof(frame));
}
