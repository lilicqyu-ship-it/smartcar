/* CPU0 diagnostic sensor streams for the phone (pure C99, no hardware). */
#ifndef APP_SENSOR_STREAM_H
#define APP_SENSOR_STREAM_H
#include <stdint.h>
/* Two new SF EVT CIDs alongside FUSION_EVT_CID (0x27) and the diag channels
 * (0x28/0x29), carried by the low-priority data ring (XCORE_dataEvtPush),
 * so a stalled link never delays the driving guard or the event outbox.
 * Consumers: esp32c6_car bridge.c pump_link_frame() DIAG dispatch emits
 * {"t":"tofz"} / {"t":"imu"} JSON; ios_remote TextMessage.swift parses them
 * for the sensor visualisation tab. */
#define SENSORSTREAM_EVT_CID_TOF_ZONES 0x2Au
#define SENSORSTREAM_EVT_CID_IMU       0x2Bu

/* ToF zone frame, fragmented into 3 EVT payloads (the cross-core data ring
 * caps one frame at XCORE_EVT_MAX_PAYLOAD = 32 B):
 *   {u16 seq, u8 frag, u8 mode, u8 valid, u16 nearestMm, u8 zone[25]}
 * Fragments 0/1 carry 25 zones each, fragment 2 the last 14 (zero padded).
 * The header repeats in every fragment so the phone can key on {seq, frag}
 * without assuming in-order delivery. zone = distanceMm/16, so 0..255 covers
 * 0..4080 mm - wider than the VL53L5CX can range; 0xFF = no trusted target
 * (driver status not 5/9). mode is a FUSION_MODE_* scene classification,
 * sampled by the caller from the running fusion. */
#define SENSORSTREAM_TOF_FRAG_ZONES 25u
#define SENSORSTREAM_TOF_FRAG_COUNT 3u
#define SENSORSTREAM_TOF_HDR_LEN    7u
#define SENSORSTREAM_TOF_WIRE_LEN   32u
#define SENSORSTREAM_ZONE_INVALID   0xFFu

/* IMU stream, one EVT per sample at 20 Hz:
 *   {u32 seq, u32 stampMs, i16 tempCentiC, i16 accMg[3], i32 gyroMdps[3]}
 * All little endian, straight from the CPU1 XcoreImu snapshot. */
#define SENSORSTREAM_IMU_PERIOD_MS 50u
#define SENSORSTREAM_IMU_WIRE_LEN  28u

/* Call from the CPU0 robot task. tofMode is the current FUSION_MODE_* scene
 * (g_driveFusion.tofMode), stamped into every zone fragment header. */
void SENSORSTREAM_tick(uint32_t nowMs, uint8_t tofMode);
#endif
