/*
 * calib_record.h - bench calibration record: wire/DFlash codec (doc 34 SS8/SS9)
 *
 * Pure C99 like sf_frame.h: stdint/string only, no OS/iLLD headers, compiles
 * verbatim for TriCore and the host unit tests (test/host/test_sf.c).
 * All multi-byte wire fields are EXPLICIT LITTLE-ENDIAN (doc 34 SS3.1); the
 * crc16 rides the SF frame codec's SF_crc16, no second polynomial.
 *
 * Owners:
 *   - the 20 B DFlash blob layout (doc 34 SS8.1) is shared with mw/calib/
 *     calib_store.c (CPU0, the only core that programs DFlash);
 *   - the EVT 0x22 / 0x23 payload builders (doc 34 SS3.1 / SS9.1) are shared
 *     with CPU0's event push and are byte-identical to esp32c6_car doc 17 SS8.4.
 */
#ifndef CALIB_RECORD_H
#define CALIB_RECORD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CALIB_REC_WHEELS        4u
#define CALIB_REC_BLOB_LEN      28u    /* v2 DFlash record incl. CRC16          */
#define CALIB_REC_SET_LEN       12u    /* 0x73 REC_SET body (after the op byte) */
#define CALIB_IMU_SET_LEN        5u    /* 0x7A {axis i8x3, trackMm u16LE}       */
#define CALIB_JOG_LEN            3u    /* 0x71 MOTOR_JOG body (after the op)    */
/* EVT 0x22 is {op, status, invert i8x4, delta i32x4, saved} = 23 B. The
 * offsets are those the slave decoder reads (esp32c6_car
 * components/c6_bridge/bridge.c:bridge_emit_cal: delta at [6..21], saved at
 * [22], minimum length 22 with saved optional). doc 34 SS3.1's "26 B with
 * delta i32x4 at [6..25]" is an arithmetic slip - four i32 are 16 bytes. */
#define CALIB_EVT_RESULT_LEN    23u
#define CALIB_EVT_RESULT_SAVED  22u    /* index of the saved byte          */
#define CALIB_EVT_REC_LEN       21u    /* EVT 0x23 incl. IMU axes/track/saved */

/* Record layout versions (doc 34 SS8.1 ver byte). */
#define CALIB_REC_VER           2u

/* Data source (doc 34 SS8.1 src byte, mirrored into EVT 0x23). */
#define CALIB_SRC_DEFAULT      0u      /* no valid record: compile-time defaults */
#define CALIB_SRC_DFLASH       1u      /* loaded from / written to DFlash        */
#define CALIB_SRC_ONLINE       2u      /* set at runtime via 0x73 REC_SET        */

/* EVT 0x22 status byte (doc 34 SS3.1) */
#define CALIB_STATUS_DONE      0u      /* all four wheels pulsed                  */
#define CALIB_STATUS_ABORTED   1u      /* e-stop mid-run                          */
#define CALIB_STATUS_BUSY      2u      /* request while a run was in progress     */

/* EVT 0x22 saved byte (doc 34 SS9.4) */
#define CALIB_SAVED_NONE       0u      /* not persisted (no run / not status 0)   */
#define CALIB_SAVED_WRITTEN    1u      /* DFlash write verified                   */
#define CALIB_SAVED_FAILED     2u      /* DFlash write attempted and failed       */

/* 0x71 jog duty domain: percent*10, clamped by the receiver (doc 34 SS9.1) */
#define CALIB_JOG_DUTY_MAX     500

/* Physical parameter ranges (doc 34 SS8.1); out of range == invalid record. */
#define CALIB_FULLSCALE_MIN    100
#define CALIB_FULLSCALE_MAX    5000
#define CALIB_FULLSCALE_DEF    1000
#define CALIB_WHEELDIA_MIN     30
#define CALIB_WHEELDIA_MAX     200
#define CALIB_WHEELDIA_DEF     48      /* MG310 kit tyre (doc 34 SS8.1) */

/* Motor position metadata (doc 34 SS8.1 pos byte; display/UI only in v1). */
#define CALIB_POS_FRONT_LEFT   0u
#define CALIB_POS_FRONT_RIGHT  1u
#define CALIB_POS_REAR_LEFT    2u
#define CALIB_POS_REAR_RIGHT   3u

/* In-memory record state (doc 34 SS9.2 XcoreCalibRecord): no magic/crc. */
typedef struct
{
    uint8_t  ver;                            /* CALIB_REC_VER                   */
    uint8_t  src;                            /* CALIB_SRC_*                     */
    uint8_t  pos[CALIB_REC_WHEELS];          /* CALIB_POS_*, A..D metadata      */
    int8_t   invert[CALIB_REC_WHEELS];       /* +1/-1, 0x70 result             */
    int16_t  fullScaleMmS;                   /* percent*10 == 1000 at this mm/s */
    int16_t  wheelDiaMm;                     /* tyre diameter, mm               */
    int8_t   imuAxis[3];                     /* ±1..±3, all zero=uncalibrated  */
    uint16_t trackMm;                        /* 80..600, 0=uncalibrated        */
} CalibRecord;

/* Compile-time defaults: the factory state (pos per doc 23 SS3 motor table:
 * A front-left, B rear-left, C rear-right, D front-right). */
void CALIBREC_fillDefaults(CalibRecord *rec);

/* Field-level validation (doc 34 SS8.1 range checks; a violation is treated
 * exactly like a CRC failure by the loader). */
uint8_t CALIBREC_paramsOk(const CalibRecord *rec);   /* 1 = all in range       */

/* --- 0x71 MOTOR_JOG body {motor u8, duty i16} ------------------------------- */
uint8_t CALIBREC_jogDecode(const uint8_t *p, uint8_t *motor, int16_t *duty);

/* --- 0x73 REC_SET body {pos u8x4, invert i8x4, fullScale i16, wheelDia i16} - */
uint8_t CALIBREC_recSetDecode(const uint8_t *p, uint8_t len, CalibRecord *rec);

/* 0x7A: validates a right-handed IMU axis map and measured wheel track. */
uint8_t CALIBREC_imuSetDecode(const uint8_t *p, uint8_t len, CalibRecord *rec);

/* --- EVT 0x22 {op, status, invert i8x4, delta i32x4, saved} ----------------- */
void CALIBREC_buildEvtResult(uint8_t *buf, uint8_t status,
                             const int8_t invert[CALIB_REC_WHEELS],
                             const int32_t delta[CALIB_REC_WHEELS],
                             uint8_t saved);

/* --- EVT 0x23 {ver, src, pos u8x4, invert i8x4, fullScale i16, wheelDia i16,
 *               crcOk} -------------------------------------------------------- */
void CALIBREC_buildEvtRec(uint8_t *buf, const CalibRecord *rec, uint8_t crcOk);

/* --- DFlash blob (doc 34 SS8.1): 'S','D','C','1' magic, ver, src, pos,
 * invert, fullScale, wheelDia, crc16 over bytes 0..17 ----------------------- */
void CALIBREC_encode(const CalibRecord *rec, uint8_t *blob);

/* Decodes and validates magic/version/CRC/ranges. On any failure the OUT
 * record carries the defaults with src=CALIB_SRC_DEFAULT, and 0 is returned
 * (the crcOk=0 reporting is the caller's job, doc 34 SS8.1). */
uint8_t CALIBREC_decode(const uint8_t *blob, CalibRecord *rec);

/* ---- explicit little-endian signed helpers (SF_putU* twins) ---------------- */
static inline void CALIBREC_putI16(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v & 0xFFu);
    p[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFFu);
}

static inline void CALIBREC_putI32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)((uint32_t)v & 0xFFu);
    p[1] = (uint8_t)(((uint32_t)v >> 8) & 0xFFu);
    p[2] = (uint8_t)(((uint32_t)v >> 16) & 0xFFu);
    p[3] = (uint8_t)(((uint32_t)v >> 24) & 0xFFu);
}

static inline int16_t CALIBREC_getI16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline int32_t CALIBREC_getI32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

#ifdef __cplusplus
}
#endif

#endif /* CALIB_RECORD_H */
