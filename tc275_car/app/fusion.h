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
    uint32_t lastMs, imuSeq, imuMs, encSeq, encMs, tofSeq, stillMs;
    int32_t counts[4];
    float bias[3], velocity, heading, roll, pitch, holdHeading, yawRate;
    uint16_t biasSamples;
    uint8_t started, imuSeen, encSeen, countsSeen, latched, clearFrames, holding, slip;
} Fusion;
void FUSION_init(Fusion *s);
/* CPU0 stopped-only caller. Reject non-right-handed axis maps / bad geometry. */
uint8_t FUSION_calibrate(Fusion *s, const int8_t axis[3], uint16_t trackMm);
void FUSION_step(Fusion *s, const FusionInput *in);
void FUSION_encode(const FusionOutput *o, uint8_t wire[FUSION_WIRE_LEN]);
#endif
