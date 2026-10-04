#ifndef XCORE_H
#define XCORE_H

#include "Ifx_Types.h"
#include "mw/proto/protocol.h"
#include "mw/calib/calib_record.h"
#include "app/fusion.h"

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
 * alive = "any side produced an edge within the alive window" - a movement
 * indicator, not per-side health (a dead side hides behind a moving opposite
 * side, a parked robot reads FALSE with healthy sensors). edgeAgeMs is the
 * per-side health primitive (doc 51); consumers combine it with the side
 * command. CPU1 publishes; CPU0 and CPU2 read. */
typedef struct
{
    uint32 seq;              /* publication freshness, including stationary */
    sint16  pctLeft;          /* -1000..+1000, percent*10 telemetry domain  */
    sint16  pctRight;
    sint16  vMeasLeftMmS;     /* physical mm/s, SF telemetry vMeasL         */
    sint16  vMeasRightMmS;
    uint32  odoLeftMm;        /* per-side absolute distance since boot, mm  */
    uint32  odoRightMm;
    boolean alive;
    uint16  edgeAgeMs[2];     /* left/right: ms since the side's last edge,
                               * saturated 0xFFFF (= none since boot)       */
    sint32  raw[4];           /* E1..E4 (= MOTOR_A..D) post-invert x4 counts,
                               * cumulative since boot; jog count EVT 0x26  */
} XcoreEncoder;

void    XCORE_encoderPublish(const XcoreEncoder *enc);
void    XCORE_encoderRead(XcoreEncoder *enc);
boolean XCORE_encoderIsAlive(void);

/* Direct e-stop bypass set by CPU2, cleared by CPU0 on fault clear/reset */
void XCORE_estopRequest(void);
void XCORE_estopClear(void);
boolean XCORE_estopIsActive(void);

/* Battery voltage (CPU1 ADC -> telemetry; doc 23 section 6: D24A J6-1
 * divider on AN4/X2-23). CPU1 publishes the filtered VIN in mV; CPU2 puts it
 * into the SF telemetry batteryMv/batteryPct fields, CPU0's future under-
 * voltage guard (doc 21 SS5.2) reads the same block. 0 = not measured yet. */
void   XCORE_battSetMv(uint16 mv);        /* CPU1 only                          */
uint16 XCORE_battGetMv(void);             /* any core                           */

/* Six-axis IMU sample (CPU1 bsp/imu -> CPU0/CPU2, doc 35): the last burst the
 * 1 kHz task converted, in display-ready unit domains. seq bumps on every
 * publish (XCORE_init zero state = "never published", so consumers watch seq
 * instead of guessing from the data). drdyCount/errCount ride along for the
 * bench lines; alive is WHO_AM_I ok AND reads succeeding. */
typedef struct
{
    uint32  seq;                          /* bumped by XCORE_imuPublish       */
    uint32  stampMs;                      /* CPU1 sampling time, STM ms       */
    boolean alive;
    uint8   whoAmI;                       /* last probe result (0x71 expected)*/
    sint16  accMilliG[3];                 /* X/Y/Z, mg                        */
    sint32  gyroMilliDps[3];              /* X/Y/Z, mdps                      */
    sint16  tempCentiC;                   /* die temperature, 0.01 degC       */
    uint32  drdyCount;                    /* INT1 rising edges since boot     */
    uint32  errCount;                     /* failed SPI transactions          */
} XcoreImu;

void XCORE_imuPublish(const XcoreImu *imu);   /* CPU1 only                    */
void XCORE_imuRead(XcoreImu *imu);            /* any core                     */

void XCORE_tofPublish(const FusionTof *tof);
void XCORE_tofRead(FusionTof *tof);
void XCORE_fusionPublish(const FusionOutput *out);
void XCORE_fusionRead(FusionOutput *out);

/* Bench wheel-direction calibration request (doc 23 section 8.4): CPU0
 * latches it on PROTO 0x70, CPU1 consumes it once and runs the per-wheel
 * pulse test on the same core that owns the motors. */
void    XCORE_dirCalibRequest(void);          /* CPU0: latch one request     */
boolean XCORE_dirCalibConsume(void);          /* CPU1: TRUE once, then clear */

/* Calibration result (doc 34 SS3.2): CPU1 publishes exactly one block per
 * run / reject, CPU0 consumes it (mw/calib/calib_store.c:calib_handleResult is
 * the single consumer; it turns the block into the EVT 0x22 frame on the
 * outbox below, so one run produces precisely one frame). Unmeasured wheels
 * carry delta 0. */
typedef struct
{
    uint8  pending;                           /* 1 = result not yet taken     */
    uint8  status;                            /* CALIB_STATUS_*               */
    sint8  invert[CALIB_REC_WHEELS];          /* per-wheel sign after the run */
    sint32 delta[CALIB_REC_WHEELS];           /* pulse count deltas           */
} XcoreCalibResult;

void    XCORE_calibResultPublish(const XcoreCalibResult *res); /* CPU1 only */
boolean XCORE_calibResultTake(XcoreCalibResult *res);          /* CPU0 only */

/* Per-motor open-loop jog, 0x71 (doc 34 SS9.3): CPU0 writes duty after
 * clamping to +-CALIB_JOG_DUTY_MAX and fault-gating, CPU1's 1 kHz loop
 * consumes it. jogSeq advances on every accepted command; CPU1 treats jog as
 * active for 300 ms after a change and zeroes the duties when it stops
 * hearing (newest-wins mailbox, no queue). */
typedef struct
{
    sint16 duty[CALIB_REC_WHEELS];            /* percent*10, +-500 clamped     */
    uint32 jogSeq;
} XcoreJog;

void    XCORE_jogSet(uint8 motor, sint16 duty);   /* CPU0 only              */
uint32  XCORE_jogGet(XcoreJog *jog);              /* CPU1: returns jogSeq   */
void    XCORE_jogClear(void);                     /* CPU1 on estop/timeout  */

/* Live calibration record (doc 34 SS8/SS9.2): CPU0 loads it from DFlash at
 * boot and updates it on 0x73/0x74; CPU1 applies it in its 1 kHz loop
 * (version change -> invert + encoder speed-conversion parameters), CPU2
 * reads it for the vTarget mm/s conversion. The version counter rides the
 * whole struct: it never repeats, so a consumer can poll cheaply and never
 * miss an update. */
typedef struct
{
    uint8       version;                      /* bumped on every set */
    CalibRecord rec;
} XcoreRecordLive;

void XCORE_recordGet(XcoreRecordLive *out);   /* any core                       */
void XCORE_recordSet(const CalibRecord *rec); /* CPU0 only; bumps version       */

/* Robot status block: CPU0 publishes every 10 ms, CPU2 answers GET_STATUS / HTTP */
void XCORE_statusPublish(const ProtocolStatus *status);
void XCORE_statusGet(ProtocolStatus *status);

/* Event outbox: CPU0 pushes SF TYPE_EVT frames (DPT results, doc 34 SS9.4),
 * CPU2 is the only sender and pops in push order. peek+pop are separate so
 * a full TX queue can retry the SAME frame next tick instead of dropping it
 * (doc 34 SS3.4: send failure keeps the frame pending). */
#define XCORE_EVT_MAX_PAYLOAD 32u
#define XCORE_EVT_QUEUE_LEN    8u

typedef struct
{
    uint8 type;
    uint8 cid;
    uint8 len;                                /* <= XCORE_EVT_MAX_PAYLOAD     */
    uint8 payload[XCORE_EVT_MAX_PAYLOAD];
} XcoreEvtFrame;

boolean XCORE_evtPush(const XcoreEvtFrame *frame);  /* CPU0                    */
boolean XCORE_evtPeek(XcoreEvtFrame *frame);        /* CPU2: head, keep it     */
void    XCORE_evtPop(void);                         /* CPU2: after send OK     */

/* Command queue: CPU2 pushes decoded frames, CPU0 consumes in the control task */
boolean XCORE_cmdPush(const XcoreCmdMsg *msg);
boolean XCORE_cmdPop(XcoreCmdMsg *msg);

/* Diagnostics never consume the driving queue or control event slots. */
boolean XCORE_diagCmdPush(const XcoreCmdMsg *msg); /* CPU2, depth 4 */
boolean XCORE_diagCmdPeek(XcoreCmdMsg *msg);      /* CPU0, retain on backpressure */
void XCORE_diagCmdPop(void);
boolean XCORE_dataEvtPush(const XcoreEvtFrame *frame); /* CPU0, depth 8 */
boolean XCORE_dataEvtPeek(XcoreEvtFrame *frame);        /* CPU2 */
void XCORE_dataEvtPop(void);
void XCORE_linkPublish(boolean up, uint32 stampMs); /* CPU2 */
boolean XCORE_linkRead(uint32 *stampMs);              /* CPU0 */
void XCORE_benchSetActive(boolean active); /* CPU1 calibration ownership */
boolean XCORE_benchIsActive(void);

/* Newest-wins variant of XCORE_cmdPush: if a message with the same cmd byte is
 * still queued, it is overwritten in place (the newest of them, so CPU0
 * applies this message's data last) and nothing is appended. Meant for
 * periodic state commands where every queued copy is superseded by the one
 * arriving now (the 30 Hz SET_SPEED joystick/heartbeat stream): a burst of
 * stale copies collapses into one entry instead of filling the queue and
 * being rejected. One-shot and safety commands (e-stop) must use the plain
 * push - their ordering is their meaning. */
boolean XCORE_cmdPushLatest(const XcoreCmdMsg *msg);

/* Log bridge: CPU1/CPU2 write lines into a shared ring, CPU0 drains to UART */
void XCORE_log(const char *s);            /* append without newline */
void XCORE_logln(const char *s);          /* append one line */
void XCORE_logService(void);              /* CPU0 only: non-blocking byte pump
                                           * into the UART software TX FIFO */

/* Structured diagnostics: named units, exact u32/i32, hex registers and text.
 * Formatting occurs outside the mutex. Long records split at FIELD boundaries,
 * repeating the tag; no field is silently dropped to satisfy the line limit. */
typedef struct {
    const char *name;
    uint32 number;
    const char *text;
    uint8 kind; /* 0=u32, 1=i32, 2=hex32, 3=text */
} XcoreLogField;
/* TASKING 6.x requires constant aggregate initializers. Assign runtime fields
 * individually; determine the exact array length at preprocessing time. */
static inline void XCORE_logFieldSet(XcoreLogField *fields, uint8 *count,
    const char *name, uint32 number, const char *text, uint8 kind)
{
    XcoreLogField *field = &fields[(*count)++];
    field->name = name; field->number = number; field->text = text; field->kind = kind;
}
#define XL_U(name, value) XCORE_logFieldSet(logFields, &logCount, name, (uint32)(value), NULL_PTR, 0u)
#define XL_I(name, value) XCORE_logFieldSet(logFields, &logCount, name, (uint32)(sint32)(value), NULL_PTR, 1u)
#define XL_H(name, value) XCORE_logFieldSet(logFields, &logCount, name, (uint32)(value), NULL_PTR, 2u)
#define XL_S(name, value) XCORE_logFieldSet(logFields, &logCount, name, 0u, value, 3u)
#define XCORE_LOG_COUNT_(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,count,...) count
#define XCORE_LOG_COUNT(...) XCORE_LOG_COUNT_(__VA_ARGS__,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)
#define XCORE_LOG_FIELDS(tag, ...) do { \
    XcoreLogField logFields[XCORE_LOG_COUNT(__VA_ARGS__)]; \
    uint8 logCount = 0u; \
    (void)(__VA_ARGS__); \
    XCORE_logFields(tag, logFields, logCount); \
} while (0)
void XCORE_logFields(const char *tag, const XcoreLogField *fields, uint8 count);
uint32 XCORE_logDropped(void); /* discarded WHOLE lines since XCORE_init */

/* Caller-owned rate gate. Unsigned elapsed arithmetic tolerates clock wrap.
 * Counters and raw sensor values belong in periodic summaries, not signatures.
 * urgent is a NEW fault/stop transition, never a persistent fault level. */
typedef struct { uint32 lastMs, signature; boolean initialized; } XcoreLogGate;
static inline boolean XCORE_logDue(XcoreLogGate *g, uint32 nowMs, uint32 signature,
                                   uint32 periodMs, uint32 changeMinMs, boolean urgent)
{
    uint32 elapsed = nowMs - g->lastMs;
    if (!g->initialized || elapsed >= periodMs ||
        ((signature != g->signature) && (urgent || elapsed >= changeMinMs))) {
        g->initialized = TRUE; g->lastMs = nowMs; g->signature = signature;
        return TRUE;
    }
    return FALSE;
}

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
