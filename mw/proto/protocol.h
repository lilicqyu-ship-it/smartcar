#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "Ifx_Types.h"

/* Frame: AA 55 CMD LEN DATA... CRC */
#define PROTO_HEADER1      0xAA
#define PROTO_HEADER2      0x55
#define PROTO_MAX_PAYLOAD  16
#define PROTO_CRC_SEED     0x00

/* Commands (requirement.md section 19) */
#define PROTO_CMD_STOP            0x01
#define PROTO_CMD_FORWARD         0x02
#define PROTO_CMD_BACKWARD        0x03
#define PROTO_CMD_LEFT            0x04
#define PROTO_CMD_RIGHT           0x05
#define PROTO_CMD_FORWARD_LEFT    0x06
#define PROTO_CMD_FORWARD_RIGHT   0x07
#define PROTO_CMD_ROTATE_LEFT     0x08
#define PROTO_CMD_ROTATE_RIGHT    0x09
#define PROTO_CMD_SET_SPEED       0x10
#define PROTO_CMD_GET_STATUS      0x20
#define PROTO_CMD_HEARTBEAT       0x21
#define PROTO_CMD_RESET           0x30
#define PROTO_CMD_CLEAR_FAULT     0x31
#define PROTO_CMD_EMERGENCY_STOP  0x32

/* Response: same CMD echoed back, DATA carries result */
#define PROTO_CMD_STATUS_REPLY    0x40

/* Robot states (requirement.md section 16) */
#define ROBOT_STATE_INIT           0x00
#define ROBOT_STATE_IDLE           0x01
#define ROBOT_STATE_FORWARD        0x02
#define ROBOT_STATE_BACKWARD       0x03
#define ROBOT_STATE_LEFT           0x04
#define ROBOT_STATE_RIGHT          0x05
#define ROBOT_STATE_FORWARD_LEFT   0x06
#define ROBOT_STATE_FORWARD_RIGHT  0x07
#define ROBOT_STATE_ROTATE_LEFT    0x08
#define ROBOT_STATE_ROTATE_RIGHT   0x09
#define ROBOT_STATE_FAULT          0x0A

/* Status reply payload layout (sent to host via the ESP32-C6 WiFi link) */
typedef struct
{
    uint8  state;             /* ROBOT_STATE_* */
    sint8  leftSpeed;         /* -100..+100 */
    sint8  rightSpeed;        /* -100..+100 */
    uint8  heartbeatOk;       /* 1 = heartbeat within timeout */
    uint8  faultCode;         /* 0 = no fault */
    uint8  emergencyStop;     /* 1 = e-stop active */
} ProtocolStatus;

/* Parser state machine outcome */
typedef enum
{
    PROTO_RESULT_IDLE = 0,
    PROTO_RESULT_FRAME,       /* a complete valid frame was dispatched */
    PROTO_RESULT_ERROR
} ProtoResult;

void  PROTO_init(void);
void  PROTO_feedByte(uint8 byte);                     /* CPU2: feed one byte from the WiFi UART */
void  PROTO_handleCommand(uint8 cmd, const uint8 *data, uint8 len); /* CPU0: execute a validated command */
ProtoResult PROTO_process(void);                      /* non-blocking pump, returns last dispatch result */
void  PROTO_sendStatus(const ProtocolStatus *status); /* build status frame and send to the WiFi UART (CPU2) */
boolean PROTO_sendBytes(const uint8 *data, uint32 len); /* raw write to the WiFi UART (CPU2, for host bridge) */

#endif