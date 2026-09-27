#ifndef XCORE_H
#define XCORE_H

#include "Ifx_Types.h"
#include "mw/proto/protocol.h"

/* Cross-core shared memory for the 3-core partition:
 *   CPU0 (FreeRTOS) : robot control task
 *   CPU1 (bare)     : motor algorithm
 *   CPU2 (bare)     : ESP32-C6 AT WiFi link
 * All blocks live in shared data RAM (TC275 has no data cache, so no cache
 * maintenance is needed) and are guarded by one tiny hardware-swap spinlock.
 * Call XCORE_init() once on CPU0 before the IfxCpu sync event is released. */
void XCORE_init(void);

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

/* Measured wheel speeds (CPU1 encoder -> telemetry). One snapshot, two unit
 * domains: pct*10 (-1000..+1000) for the demo status overlay, physical mm/s
 * and per-side odometer for the SF telemetry (SDD §6.3 vMeasL/R, odoSession).
 * alive = encoder edges seen within the alive window. CPU1 publishes; CPU0
 * and CPU2 read. */
typedef struct
{
    sint16  pctLeft;          /* -1000..+1000, percent*10 telemetry domain  */
    sint16  pctRight;
    sint16  vMeasLeftMmS;     /* physical mm/s, SF telemetry vMeasL         */
    sint16  vMeasRightMmS;
    uint32  odoLeftMm;        /* per-side absolute distance since boot, mm  */
    uint32  odoRightMm;
    boolean alive;
} XcoreEncoder;

void    XCORE_encoderPublish(const XcoreEncoder *enc);
void    XCORE_encoderRead(XcoreEncoder *enc);
boolean XCORE_encoderIsAlive(void);

/* Direct e-stop bypass set by CPU2, cleared by CPU0 on fault clear/reset */
void XCORE_estopRequest(void);
void XCORE_estopClear(void);
boolean XCORE_estopIsActive(void);

/* Bench wheel-direction calibration request (doc 23 section 8.4): CPU0
 * latches it on PROTO 0x70, CPU1 consumes it once and runs the per-wheel
 * pulse test on the same core that owns the motors. The result is reported
 * on the console log ("ENCCAL=..."), not back through shared memory - the
 * audience is a human on the bench, not another task. */
void    XCORE_dirCalibRequest(void);          /* CPU0: latch one request     */
boolean XCORE_dirCalibConsume(void);          /* CPU1: TRUE once, then clear */

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

/* Formatted diagnostic line for cores with no printf (CPU1/CPU2): emits
 * "label=v0 v1 v2 ..." as one line, each value in unsigned decimal. Meant for
 * low-rate bench observation (e.g. the SPI link state), so the value count is
 * capped and a line that would not fit the ring is dropped whole, exactly like
 * XCORE_log(). Pass n = 0 to print the label alone. */
#define XCORE_LOG_MAX_VALS   20u
void XCORE_logu(const char *label, const uint32 *vals, uint8 n);

/* Signed twin of XCORE_logu: same line format, each value in decimal with a
 * '-' when negative (e.g. reverse wheel speed). */
void XCORE_logi(const char *label, const sint32 *vals, uint8 n);

#endif
