#ifndef XCORE_H
#define XCORE_H

#include "Ifx_Types.h"
#include "protocol.h"

/* Cross-core shared memory for the 3-core partition:
 *   CPU0 (FreeRTOS) : robot control task
 *   CPU1 (bare)     : motor algorithm
 *   CPU2 (bare)     : ESP32-C6 AT WiFi link
 * All blocks live in shared data RAM (TC275 has no data cache, so no cache
 * maintenance is needed) and are guarded by one tiny hardware-swap spinlock.
 * Call XCORE_init() once on CPU0 before the IfxCpu sync event is released. */

/* One decoded protocol frame, queued CPU2 -> CPU0 for execution */
typedef struct
{
    uint8 cmd;                            /* PROTO_CMD_* */
    uint8 len;                            /* payload length */
    uint8 data[PROTO_MAX_PAYLOAD];        /* payload copy */
} XcoreCmdMsg;

/* Motor targets (CPU0 control task -> CPU1 motor algorithm) */
void   XCORE_motorSetTarget(sint16 left, sint16 right, boolean estop);
uint32 XCORE_motorGetTarget(sint16 *left, sint16 *right, boolean *estop); /* returns update counter */

/* Motor outputs (CPU1 motor algorithm -> telemetry) */
void XCORE_motorStatusSet(sint16 left, sint16 right);
void XCORE_motorStatusGet(sint16 *left, sint16 *right);

/* Measured wheel speeds (CPU1 encoder -> telemetry). Percent*10 domain
 * (-1000..+1000); alive = encoder edges seen within the alive window. */
void    XCORE_encoderSet(sint16 left, sint16 right, boolean alive);
boolean XCORE_encoderGet(sint16 *left, sint16 *right, boolean *alive);

/* Direct e-stop bypass set by CPU2, cleared by CPU0 on fault clear/reset */
void XCORE_estopRequest(void);
void XCORE_estopClear(void);
boolean XCORE_estopIsActive(void);

/* Robot status block: CPU0 publishes every 10 ms, CPU2 answers GET_STATUS / HTTP */
void XCORE_statusPublish(const ProtocolStatus *status);
void XCORE_statusGet(ProtocolStatus *status);

/* Command queue: CPU2 pushes decoded frames, CPU0 consumes in the control task */
boolean XCORE_cmdPush(const XcoreCmdMsg *msg);
boolean XCORE_cmdPop(XcoreCmdMsg *msg);

/* Log bridge: CPU1/CPU2 write lines into a shared ring, CPU0 drains to UART */
void XCORE_log(const char *s);            /* append without newline */
void XCORE_logln(const char *s);          /* append one line */
void XCORE_logService(void);              /* CPU0 only: print pending lines */

#endif
