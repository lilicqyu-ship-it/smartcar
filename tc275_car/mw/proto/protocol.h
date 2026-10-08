#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "Ifx_Types.h"

/* Shared command table (requirement.md section 19). The codes travel inside SF
 * frames on the SPI link (com/link.c) and are executed on CPU0 by
 * PROTO_handleCommand; PROTO_MAX_PAYLOAD also sizes the link command queue. */
#define PROTO_MAX_PAYLOAD  16

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

/* Production-test commands (DPT channel, doc 21 SS6.2 / 23 SS8.4 / 34 SS9):
 * CPU1 runs the automated wheel-direction pulse test - WHEELS OFF THE GROUND,
 * ~1.4 s, result on the console log AND on EVT 0x22. 0x71 jogs one motor open
 * loop from the bench; 0x72/0x73/0x74 read, write and clear the calibration
 * record in DFlash (doc 34 SS8), echoed on EVT 0x23. They arrive as SF CMD/DPT
 * ops, which link.c forwards verbatim; the phone UI reaches them through the
 * calibration page (esp32c6_car doc 17). */
#define PROTO_CMD_DPT_CAL_DIR     0x70
#define PROTO_CMD_DPT_MOTOR_JOG   0x71
#define PROTO_CMD_DPT_REC_GET     0x72
#define PROTO_CMD_DPT_REC_SET     0x73
#define PROTO_CMD_DPT_REC_CLEAR   0x74
#define PROTO_CMD_DPT_IMU_CAL_SET 0x75

/* OTA commands (doc 24 SS5.3 / F7 command table; values match the C6 side,
 * esp32c6_car components/c6_proto/proto_frames.h). They travel as SF OTA frames
 * and the SF layer forwards them in link.c - the constants complete the shared
 * command table. */
#define PROTO_CMD_OTA_BEGIN       0x60  /* {u32 total, u32 crc32}            */
#define PROTO_CMD_OTA_CHUNK       0x61  /* {u16 idx, data<=240}              */
#define PROTO_CMD_OTA_ACK         0x62  /* {u16 idx, u8 result}              */
#define PROTO_CMD_OTA_STATUS      0x63  /* {u8 state, u8 pct}                */
#define PROTO_CMD_OTA_SWAP        0x64  /* TC275 reboots into the new slot   */
#define PROTO_CMD_OTA_ABORT       0x65  /* drop the half-written slot        */

/* DIAG family (SF CMD / CID_DIAG {u8 op, ...}, link.c forwards op + rest).
 * Sub-op 0x24 = version request: S3 {"t":"tcver"} -> C6 queues it and pulls
 * the IRQ line -> this MCU reads it over SPI and answers at once with the
 * EVT 0x24/0x25 version beacons instead of waiting for the 5 s period. */
#define PROTO_CMD_DIAG            0x53
#define PROTO_DIAG_SUB_VER_REQ    0x24

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

/* Status block published by CPU0 and carried to the host in the 20 ms SF
 * telemetry frame */
typedef struct
{
    uint8  state;             /* ROBOT_STATE_* */
    sint8  leftSpeed;         /* -100..+100 */
    sint8  rightSpeed;        /* -100..+100 */
    uint8  heartbeatOk;       /* 1 = heartbeat within timeout */
    uint8  faultCode;         /* 0 = no fault */
    uint8  emergencyStop;     /* 1 = e-stop active */
} ProtocolStatus;

/* CPU0: execute a command byte the link layer already validated */
void  PROTO_handleCommand(uint8 cmd, const uint8 *data, uint8 len);

#endif
