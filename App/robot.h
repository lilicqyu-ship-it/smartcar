#ifndef ROBOT_H
#define ROBOT_H

#include "Ifx_Types.h"
#include "protocol.h"

#define ROBOT_HEARTBEAT_TIMEOUT_MS  100U   /* requirement section 22 */
#define ROBOT_DEFAULT_SPEED         50     /* initial S for motion commands */

typedef struct
{
    sint8 left;    /* -100..+100 */
    sint8 right;   /* -100..+100 */
} RobotSpeeds;

void ROBOT_init(void);

/* Command entry points (called by protocol parser) */
void ROBOT_cmdStop(void);
void ROBOT_cmdMotion(uint8 cmd);               /* FORWARD/BACKWARD/LEFT/... uses stored S */
void ROBOT_cmdSetSpeed(sint8 speed);           /* SET_SPEED: S = |speed| */
void ROBOT_cmdSetSpeeds(sint8 left, sint8 right); /* direct left/right command */
void ROBOT_cmdHeartbeat(void);
void ROBOT_cmdEmergencyStop(void);
void ROBOT_cmdClearFault(void);
void ROBOT_cmdReset(void);

/* Periodic (10 ms task) */
void ROBOT_task(void);

/* Status access */
uint8  ROBOT_getState(void);
uint8  ROBOT_getFaultCode(void);
boolean ROBOT_isEmergencyStop(void);
boolean ROBOT_isHeartbeatOk(void);
RobotSpeeds ROBOT_getSpeeds(void);

#endif