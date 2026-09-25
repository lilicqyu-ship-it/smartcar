#include "protocol.h"
#include "robot.h"
#include "esp8266.h"

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

static void PROTO_dispatch(uint8 cmd, const uint8 *data, uint8 len)
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

    case PROTO_CMD_GET_STATUS:
    {
        ProtocolStatus status;

        status.state         = ROBOT_getState();
        status.leftSpeed     = (sint8)ROBOT_getSpeeds().left;
        status.rightSpeed    = (sint8)ROBOT_getSpeeds().right;
        status.heartbeatOk   = ROBOT_isHeartbeatOk() ? 1 : 0;
        status.faultCode     = ROBOT_getFaultCode();
        status.emergencyStop = ROBOT_isEmergencyStop() ? 1 : 0;
        PROTO_sendStatus(&status);
        break;
    }

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
            PROTO_dispatch(g_parser.cmd, g_parser.payload, g_parser.len);
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
    return ESP8266_sendRaw(data, len);
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