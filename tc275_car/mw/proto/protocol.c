/* Command execution on CPU0 - the robot state owner.
 *
 * The transport is the SF-over-SPI link (com/link.c): it validates each frame,
 * answers GET_STATUS out of the status block CPU0 publishes, and queues the
 * remaining ops to CPU0 through the xcore command queue. PROTO_handleCommand
 * runs on that drained queue from the 10 ms control task, so it only ever sees
 * command bytes that the link layer already accepted.
 *
 * protocol.h carries the shared command table (same values as the C6 side);
 * frame sync and CRC belong to SF, not to this file. */
#include "mw/proto/protocol.h"
#include "app/robot.h"
#include "mw/calib/calib_store.h"
#include "mw/xcore/xcore.h"
#include "mw/app_version.h"

/* CPU0 only: execute a validated command against the robot controller */
void PROTO_handleCommand(uint8 cmd, const uint8 *data, uint8 len)
{
    switch (cmd)
    {
    case PROTO_CMD_STOP:
        /* Any driving command doubles as the heartbeat (doc 21 SS6.2 for
         * 0x50): without this the 100 ms ROBOT watchdog keeps zeroing the
         * wheel targets between commands of a joystick stream that arrives
         * as SET_SPEED/DRIVE frames and never as an explicit 0x21. */
        ROBOT_cmdHeartbeat();
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
        ROBOT_cmdHeartbeat();
        ROBOT_cmdMotion(cmd);
        break;

    case PROTO_CMD_SET_SPEED:
        ROBOT_cmdHeartbeat();
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

    case PROTO_CMD_DPT_CAL_DIR:
        /* Bench-only (wheels off ground): latch the request, CPU1's 1 kHz
         * loop picks it up and answers on the console log AND on EVT 0x22.
         * Not a driving command, so no heartbeat and no fault gating - an
         * e-stop on CPU1 aborts the run from its own branch anyway. */
        XCORE_dirCalibRequest();
        break;

    case PROTO_CMD_DPT_MOTOR_JOG:
    {
        uint8  motor;
        sint16 duty;

        /* Driving-class bench tool (doc 34 SS9.1): fault-latched or e-stopped
         * it is refused, and it never feeds the heartbeat, so a jog stream on
         * its own cannot keep the drive alive. CPU1 owns the 300 ms freshness
         * timeout and the duty clamp lives here. */
        if ((len != CALIB_JOG_LEN) ||
            (CALIBREC_jogDecode(data, &motor, &duty) == 0u))
        {
            XCORE_logln("JOG rejected (bad payload)");
            break;
        }
        if ((ROBOT_getFaultCode() != 0u) || ROBOT_isEmergencyStop())
        {
            XCORE_logln("JOG rejected (fault)");
            break;
        }
        XCORE_jogSet(motor, duty);
        break;
    }

    case PROTO_CMD_DPT_REC_GET:
        CALIB_sendRecord();
        break;

    case PROTO_CMD_DPT_REC_SET:
        CALIB_recordSet(data, len);
        break;

    case PROTO_CMD_DPT_REC_CLEAR:
        CALIB_recordClear();
        break;

    case PROTO_CMD_DIAG:
        /* Not a driving command: no heartbeat, no fault gating. The beacon
         * itself is sent by the CPU0 task right after this queue drain. */
        if ((len >= 1u) && (data[0] == PROTO_DIAG_SUB_VER_REQ))
        {
            app_ver_request();
        }
        break;

    default:
        break;
    }
}
