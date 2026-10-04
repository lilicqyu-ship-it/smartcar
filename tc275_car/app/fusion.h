/* CPU0 sensor fusion / manual driving guard. Pure C99, no hardware or RTOS. */
#ifndef APP_FUSION_H
#define APP_FUSION_H
#include <stdint.h>
#define FUSION_MAX_ZONES 64u
#define FUSION_EVT_CID 0x27u
#define FUSION_WIRE_LEN 28u
#define FUSION_TOF_OK 1u
#define FUSION_IMU_OK 2u
#define FUSION_ENCODER_OK 4u
#define FUSION_CALIBRATED 8u
#define FUSION_SLIP 16u
#define FUSION_BIAS_READY 32u
#define FUSION_NEUTRAL_REQUIRED 64u
#define FUSION_TOF_LIMITED 128u /* fresh sparse frame: manual low-speed allowance */
#define FUSION_SPARSE_MM_S 150u
/* Per-side encoder health (doc 51). A side's edge age at or under the fresh
 * threshold is live feedback; a side asked to drive may stay silent for the
 * grace window (wheel spin-up from standstill) before it counts as dead. */
#define FUSION_ENC_EDGE_FRESH_MS 100u
#define FUSION_ENC_GRACE_MS 500u
/* Forward-envelope excursion guard. The per-side speed feeding the check is
 * an 8 ms window mean with tens of mm/s of quantisation ripple, and a sparse
 * frame flap can pinch the envelope far below the current speed without any
 * physical change - so an excursion must clear the hysteresis and hold for
 * the window before it latches a stop. A genuine shrinking envelope violates
 * it for far longer (1.2.4 introduced the hold for the sparse crawl only;
 * it now covers every mode). */
#define FUSION_OVERSPEED_HYST_MM_S 100u
#define FUSION_OVERSPEED_HOLD_MS 100u
/* Reasons are independent of the existing robot emergency-stop fault. */
enum
{
    FUSION_FREE = 0,
    FUSION_SLOW = 1,
    FUSION_OBSTACLE = 2,
    FUSION_BLIND = 3,
    FUSION_TILT = 4,
    FUSION_ENCODER_LOST = 5
};
typedef struct
{
    uint32_t seq, stampMs;
    uint32_t sampleStampMs; /* STM common clock for diagnostic acquisition */
    uint8_t alive, zones;
    int16_t distanceMm[FUSION_MAX_ZONES];
    uint8_t status[FUSION_MAX_ZONES], targets[FUSION_MAX_ZONES];
} FusionTof;
typedef struct
{
    uint32_t nowMs, imuSeq, encoderSeq;
    uint8_t imuAlive, wheelsCalibrated;
    int16_t accMg[3], wheelMmS[2], request[2], fullScaleMmS;
    int32_t gyroMdps[3], counts[4];
    uint16_t encEdgeAgeMs[2]; /* per side ms since last edge, 0xFFFF = none;
                                 encoderSeq alone only proves the CPU1
                                 publisher task, not the sensors (doc 51) */
    FusionTof tof;
} FusionInput;
typedef struct
{
    /* Body frame x=forward, y=left, z=up; signed sensor axes +/-1..3.
       All zero until calibrated. trackMm must be measured before enabling. */
    int8_t axis[3];
    uint16_t trackMm, marginMm, decelMmS2, reverseMmS;
    uint8_t straightAssist;
} FusionConfig;
typedef struct
{
    uint32_t stampMs;
    uint16_t flags, nearestMm, sectorMm[3], capMmS, tofAgeMs;
    int16_t speedMmS, yawRateCdegS, headingCdeg, rollCdeg, pitchCdeg;
    int16_t effective[2]; /* percent*10 */
    uint8_t reason, brake, validZones;
} FusionOutput;
typedef struct
{
    FusionConfig cfg;
    FusionOutput out;
    uint32_t lastMs, imuSeq, imuMs, encSeq, encMs, tofSeq, stillMs, overspeedMs;
    uint32_t capContMs;    /* stamp of the healthy frame that anchored capContMmS */
    int32_t counts[4];
    uint16_t encAbsentMs[2]; /* ms a commanded side has been edge-silent */
    uint16_t capContMmS;   /* last healthy-frame envelope; the sparse crawl
                              decays down from here at cfg.decelMmS2 instead
                              of pinching the cap in one step */
    float bias[3], velocity, heading, roll, pitch, holdHeading, yawRate;
    uint16_t biasSamples;
    uint8_t started, imuSeen, encSeen, countsSeen, latched, clearFrames, holding, slip, overspeedSeen;
} Fusion;
void FUSION_init(Fusion *s);
/* CPU0 stopped-only caller. Reject non-right-handed axis maps / bad geometry. */
uint8_t FUSION_calibrate(Fusion *s, const int8_t axis[3], uint16_t trackMm);
void FUSION_step(Fusion *s, const FusionInput *in);
void FUSION_encode(const FusionOutput *o, uint8_t wire[FUSION_WIRE_LEN]);
#endif
