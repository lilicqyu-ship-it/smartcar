/* CPU0 transaction engine: pure C99, bounded memory/work, hardware-free. */
#ifndef APP_DIAG_TXN_H
#define APP_DIAG_TXN_H
#include "mw/diag/diag_wire.h"
#include "app/fusion.h"
#define DT_RECORD_BODY 112u
#define DT_CONTROL_QUEUE 8u
#define DT_SHORT_CACHE 16u
#define DT_JOB_CACHE 4u
typedef struct {
    uint32_t nowMs, imuSeq, imuStampMs, imuErrors, tofErrors;
    int16_t accMg[3], tempCentiC;
    int32_t gyroMdps[3];
    uint8_t imuAlive, linkUp, stationary;
    uint8_t imuInfo[8]; /* expected/observed WHOAMI, ODR/FS codes, axis flag, period */
    FusionTof tof;
} DT_Input;
typedef struct {
    uint32_t session, stamp;
    uint16_t request, seq, len;
    uint8_t kind, code, index;
    uint8_t body[DT_RECORD_BODY];
} DT_Record;
typedef struct {
    uint8_t used, requestBytes[16], requestLen, recordCount;
    DT_Record records[2];
} DT_Cached;
typedef struct {
    DT_Cached cache;
    uint32_t start, end, progressMs, imuDue, imuSource, tofSource;
    uint32_t imuCount, tofCount, drops, imuErrors, tofErrors;
    int64_t tofSum, tofSquare;
    uint32_t tofValid, tofTotal;
    uint16_t nextSeq, duration, reference, tolerance;
    uint8_t sensors, hz, state, zone, verdict, terminalPending;
} DT_Job;
typedef struct {
    uint8_t body[260];
    uint32_t stamp;
    uint16_t len, seq;
    uint8_t kind, index;
} DT_Stream;
typedef struct {
    uint32_t session, leaseMs, firmwareVersion;
    uint16_t highWater;
    uint8_t active, shortNext, ctrlHead, ctrlTail, ctrlCount;
    uint8_t tofZones, tofHz;
    DT_Cached shortCache[DT_SHORT_CACHE];
    DT_Job jobs[DT_JOB_CACHE];
    DT_Record controls[DT_CONTROL_QUEUE];
    DT_Stream imuStream, tofStream;
} DT_Engine;
/* Return true only when the complete fragment was queued. */
typedef uint8_t (*DT_Emit)(void *context, uint8_t cid, const uint8_t *bytes, uint8_t len);
void DT_init(DT_Engine *s, uint32_t firmwareVersion, uint8_t tofZones, uint8_t tofHz);
/* 0 = backpressure, no mutation; caller must retain the request and retry.
 * 1 = consumed (accepted, rejected or replayed). */
uint8_t DT_submit(DT_Engine *s, const uint8_t *request, uint8_t len, const DT_Input *in);
void DT_tick(DT_Engine *s, const DT_Input *in, DT_Emit emit, void *context);
void DT_abortMotion(DT_Engine *s, const DT_Input *in);
#endif
