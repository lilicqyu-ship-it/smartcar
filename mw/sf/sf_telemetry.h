/*
 * sf_telemetry.h - the 38 byte SF telemetry payload (TYPE_TEL / CID_TELEMETRY)
 *
 * Wire truth source: doc/20-design/22-link-spi-design.md SS5.5 and SDD SS6.3.
 * The layout is byte for byte the proto v2 0x41 payload, because the C6 side
 * decodes it with its own v2 decoder instead of a second table
 * (esp32c6_car components/c6_proto/proto_frames.c:proto_telemetry_decode).
 *
 * That is also why the length is load bearing: c6_link rejects any TEL frame
 * that is not exactly CID_TELEMETRY with at least SF_TELEMETRY_LEN bytes
 * (c6_link/link.c:sf_to_v2), so a short telemetry payload is not partial data,
 * it is no data at all. The producer fills every field it has a source for and
 * zero for the rest - see the field table for which those currently are.
 *
 * Hard rules (same as sf_frame.h): pure C99, stdint/string only, no OS/iLLD
 * headers, no dynamic memory, explicit little-endian on the wire. TriCore is big
 * endian, so nothing here may be expressed as a struct cast onto bytes.
 */
#ifndef SF_TELEMETRY_H
#define SF_TELEMETRY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF_TELEMETRY_LEN 38u

/* Offsets are the contract; the names are documentation of the same bytes.
 * All multi-byte fields little endian, no alignment padding, offsets 9/11/../33
 * are deliberately unaligned.
 *   0  seq            u32  E2E counter of this producer, strictly advancing
 *   4  uptimeMs       u32
 *   8  state          u8   ROBOT_STATE_* (protocol.h)
 *   9  faultCode      u16
 *  11  vTargetLeft    i16  mm/s
 *  13  vTargetRight   i16  mm/s
 *  15  vMeasLeft      i16  mm/s
 *  17  vMeasRight     i16  mm/s
 *  19  batteryMv      u16
 *  21  batteryPct     u8
 *  22  odoSessionMm   u32
 *  26  odoTotalMm     u32
 *  30  linkRttMs      u16
 *  32  linkErrRate    u8
 *  33  fwVer          u32
 *  37  hwRev          u8
 */
typedef struct
{
    uint32_t seq;
    uint32_t uptimeMs;
    uint8_t  state;
    uint16_t faultCode;
    int16_t  vTargetLeft;
    int16_t  vTargetRight;
    int16_t  vMeasLeft;
    int16_t  vMeasRight;
    uint16_t batteryMv;
    uint8_t  batteryPct;
    uint32_t odoSessionMm;
    uint32_t odoTotalMm;
    uint16_t linkRttMs;
    uint8_t  linkErrRate;
    uint32_t fwVer;
    uint8_t  hwRev;
} SF_Telemetry;

/* Write the payload. Returns SF_TELEMETRY_LEN, or -1 on a null pointer or a
 * buffer shorter than the payload. */
int16_t SF_telemetryEncode(const SF_Telemetry *tel, uint8_t *out, uint16_t cap);

/* Read it back - the host unit tests and the C6 side use the same table.
 * Returns 1 on success, 0 when len is short or a pointer is null. */
uint8_t SF_telemetryDecode(const uint8_t *data, uint16_t len, SF_Telemetry *tel);

#ifdef __cplusplus
}
#endif

#endif /* SF_TELEMETRY_H */
