#include "app/robot.h"

#include "FreeRTOS.h"
#include "task.h"

typedef enum
{
    ROBOT_FAULT_NONE = 0,
    ROBOT_FAULT_EMERGENCY_STOP,
    ROBOT_FAULT_COMM_TIMEOUT,
    ROBOT_FAULT_INVALID_CMD
} RobotFaultCode;

typedef struct
{
    uint8       state;
    sint8       speedS;         /* stored S from SET_SPEED */
    RobotSpeeds targets;        /* desired left/right speeds */
    boolean     heartbeatOk;
    TickType_t  lastHeartbeatTick;
    boolean     emergencyStop;
    RobotFaultCode fault;
} RobotCtrl;

static RobotCtrl g_robot;

static void ROBOT_applySpeeds(sint8 left, sint8 right)
{
    /* Only updates the desired side speeds. Actual actuation happens on CPU1:
     * the control task forwards these targets through xcore and the motor
     * algorithm there ramps and drives the TB6612s. */
    g_robot.targets.left  = left;
    g_robot.targets.right = right;
}

static void ROBOT_setState(uint8 state)
{
    g_robot.state = state;
}

void ROBOT_init(void)
{
    g_robot.state           = ROBOT_STATE_INIT;
    g_robot.speedS          = ROBOT_DEFAULT_SPEED;
    g_robot.targets.left    = 0;
    g_robot.targets.right   = 0;
    g_robot.heartbeatOk     = FALSE;
    g_robot.lastHeartbeatTick = xTaskGetTickCount();
    g_robot.emergencyStop   = FALSE;
    g_robot.fault           = ROBOT_FAULT_NONE;

    ROBOT_applySpeeds(0, 0);
    ROBOT_setState(ROBOT_STATE_IDLE);
}

void ROBOT_cmdStop(void)
{
    if (g_robot.fault != ROBOT_FAULT_NONE)
    {
        return;
    }
    ROBOT_applySpeeds(0, 0);
    ROBOT_setState(ROBOT_STATE_IDLE);
}

void ROBOT_cmdMotion(uint8 cmd)
{
    sint8 S = g_robot.speedS;

    if (g_robot.fault != ROBOT_FAULT_NONE)
    {
        return;
    }

    switch (cmd)
    {
    case PROTO_CMD_FORWARD:        ROBOT_applySpeeds( S,  S); ROBOT_setState(ROBOT_STATE_FORWARD);       break;
    case PROTO_CMD_BACKWARD:       ROBOT_applySpeeds(-S, -S); ROBOT_setState(ROBOT_STATE_BACKWARD);      break;
    case PROTO_CMD_LEFT:           ROBOT_applySpeeds(-S,  S); ROBOT_setState(ROBOT_STATE_LEFT);          break;
    case PROTO_CMD_RIGHT:          ROBOT_applySpeeds( S, -S); ROBOT_setState(ROBOT_STATE_RIGHT);         break;
    case PROTO_CMD_FORWARD_LEFT:   ROBOT_applySpeeds( S / 2, S); ROBOT_setState(ROBOT_STATE_FORWARD_LEFT);   break;
    case PROTO_CMD_FORWARD_RIGHT:  ROBOT_applySpeeds( S, S / 2); ROBOT_setState(ROBOT_STATE_FORWARD_RIGHT);  break;
    case PROTO_CMD_ROTATE_LEFT:    ROBOT_applySpeeds(-S,  S); ROBOT_setState(ROBOT_STATE_ROTATE_LEFT);   break;
    case PROTO_CMD_ROTATE_RIGHT:   ROBOT_applySpeeds( S, -S); ROBOT_setState(ROBOT_STATE_ROTATE_RIGHT);  break;
    default:                       break;
    }
}

void ROBOT_cmdSetSpeed(sint8 speed)
{
    if (speed < -100)
    {
        speed = -100;
    }
    else if (speed > 100)
    {
        speed = 100;
    }
    g_robot.speedS = (speed < 0) ? (sint8)-speed : speed;
    if (g_robot.speedS == 0)
    {
        ROBOT_cmdStop();
    }
}

void ROBOT_cmdSetSpeeds(sint8 left, sint8 right)
{
    if (g_robot.fault != ROBOT_FAULT_NONE)
    {
        return;
    }
    ROBOT_applySpeeds(left, right);
    if (left == 0 && right == 0)
    {
        ROBOT_setState(ROBOT_STATE_IDLE);
    }
    else if (left == right)
    {
        ROBOT_setState(left > 0 ? ROBOT_STATE_FORWARD : ROBOT_STATE_BACKWARD);
    }
    else if (left == -right)
    {
        ROBOT_setState(left < 0 ? ROBOT_STATE_ROTATE_LEFT : ROBOT_STATE_ROTATE_RIGHT);
    }
    else
    {
        ROBOT_setState(left < right ? ROBOT_STATE_FORWARD_LEFT : ROBOT_STATE_FORWARD_RIGHT);
    }
}

void ROBOT_cmdHeartbeat(void)
{
    g_robot.lastHeartbeatTick = xTaskGetTickCount();
    g_robot.heartbeatOk       = TRUE;
}

void ROBOT_cmdEmergencyStop(void)
{
    g_robot.emergencyStop = TRUE;
    g_robot.fault         = ROBOT_FAULT_EMERGENCY_STOP;
    ROBOT_applySpeeds(0, 0);
    ROBOT_setState(ROBOT_STATE_FAULT);
}

void ROBOT_cmdClearFault(void)
{
    if (g_robot.fault == ROBOT_FAULT_NONE)
    {
        return;
    }
    g_robot.emergencyStop = FALSE;
    g_robot.fault         = ROBOT_FAULT_NONE;
    g_robot.lastHeartbeatTick = xTaskGetTickCount();
    ROBOT_applySpeeds(0, 0);
    ROBOT_setState(ROBOT_STATE_IDLE);
}

void ROBOT_cmdReset(void)
{
    ROBOT_init();
}

void ROBOT_task(void)
{
    /* Communication timeout -> auto stop (requirement section 22) */
    if ((xTaskGetTickCount() - g_robot.lastHeartbeatTick) > pdMS_TO_TICKS(ROBOT_HEARTBEAT_TIMEOUT_MS))
    {
        if (g_robot.fault == ROBOT_FAULT_NONE)
        {
            g_robot.heartbeatOk = FALSE;
            ROBOT_applySpeeds(0, 0);
            ROBOT_setState(ROBOT_STATE_IDLE);
        }
    }
}

uint8  ROBOT_getState(void)          { return g_robot.state; }
uint8  ROBOT_getFaultCode(void)      { return (uint8)g_robot.fault; }
boolean ROBOT_isEmergencyStop(void)  { return g_robot.emergencyStop; }
boolean ROBOT_isHeartbeatOk(void)    { return g_robot.heartbeatOk; }
RobotSpeeds ROBOT_getSpeeds(void)    { return g_robot.targets; }