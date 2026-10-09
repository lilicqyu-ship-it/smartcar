/* CPU0 sensor fusion / manual driving guard. Pure C99, no hardware or RTOS. */
#ifndef APP_FUSION_H
#define APP_FUSION_H
#include <stdint.h>
/* 保护主开关：1 为生产保护，0 为仅遥测的台架旁路。
 * 旁路关闭限速与近障/盲区/编码器/倾斜硬停，保留松杆立即制动。 */
#ifndef FUSION_CFG_PROTECTION
#define FUSION_CFG_PROTECTION 1
#endif
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
#define FUSION_TOF_LIMITED 128u /* 可用帧，但完全可信区不足；忽略 ToF 限速 */
#define FUSION_TOF_TRUSTED_RATIO_NUM 2u
#define FUSION_TOF_TRUSTED_RATIO_DEN 3u /* 严格 >2/3，不能用百分比舍入 */
/* 空旷场景连续帧分类仅用于诊断；覆盖不足时不限速。 */
#define FUSION_OPEN_CLEAR_FRAMES 3u
#define FUSION_OPEN_CLEAR_RATIO_PCT 70u
#define FUSION_OPEN_UNKNOWN_MAX_PCT 30u
/* Per-side encoder health (doc 51). A side's edge age at or under the fresh
 * threshold is live feedback; a side asked to drive may stay silent for the
 * grace window (wheel spin-up from standstill) before it counts as dead. */
#define FUSION_ENC_EDGE_FRESH_MS 100u
#define FUSION_ENC_GRACE_MS 500u
#define FUSION_SLOW_DISTANCE_MM 150u /* 完全可信覆盖 >2/3 时进入降速区 */
#define FUSION_STOP_DISTANCE_MM 60u /* 原始有效距离严格 <60 mm 才近障停车 */
#define FUSION_STOP_RELEASE_MM 80u  /* 停车后的距离迟滞，仍需三帧与松杆 */
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
/* ToF scene classification for diagnostics (Fusion.tofMode); not on the wire.
 * TRACKED: targets present; only fully trusted coverage >2/3 limits speed. OPEN_CLEAR: healthy
 * but empty. DEGRADED: fresh frame, uncertain scene. Both ignore ToF speed caps.
 * BLIND: no usable frame -> stop. */
enum
{
    FUSION_MODE_BLIND = 0,
    FUSION_MODE_TRACKED = 1,
    FUSION_MODE_DEGRADED = 2,
    FUSION_MODE_OPEN = 3
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
    uint16_t trackMm, reverseMmS;
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
    uint16_t encAbsentMs[2]; /* ms a commanded side has been edge-silent */
    float bias[3], velocity, heading, roll, pitch, holdHeading, yawRate;
    uint16_t biasSamples;
    uint8_t started, imuSeen, encSeen, countsSeen, latched, clearFrames, holding, slip;
    uint8_t openClearFrames; /* consecutive new frames the field read as empty */
    uint8_t tofMode;         /* last FUSION_MODE_* scene, for diagnostics only */
    uint8_t tofNoTarget, tofUnknown; /* last frame zone counts, for logging */
    uint16_t tofNearestRawMm; /* 未减状态 9 余量的有效最近距离，诊断用 */
    uint8_t tofTrustedZones;  /* 状态 5 完全可信区数，诊断用 */
} Fusion;
void FUSION_init(Fusion *s);
/* CPU0 stopped-only caller. Reject non-right-handed axis maps / bad geometry. */
uint8_t FUSION_calibrate(Fusion *s, const int8_t axis[3], uint16_t trackMm);
void FUSION_clearCalibration(Fusion *s);
void FUSION_step(Fusion *s, const FusionInput *in);
void FUSION_encode(const FusionOutput *o, uint8_t wire[FUSION_WIRE_LEN]);
#endif
