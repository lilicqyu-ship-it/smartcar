/*
 * cam_frame.h - SmartCar Camera WS frame header codec (single shared implementation)
 *
 * This file is THE single source of truth for CAM frame header encode/parse on
 * both sides of the Camera WS (/ws/camera):
 *   - s3-gateway (S3-CAM)  : camera_ws TX  (fills header, appends JPEG)
 *   - smartcar_remote (S3) : scr_cam RX    (cam_frame_parse before decode)
 * Schema source: S3CAM LLDD V1.0 §8.3 / Table 14; Remote design doc §4.1
 * (smartcar_remote/doc/SmartCar_S3Remote_详细设计说明书_V1.0.md).
 *
 * Hard rules (same conventions as contracts/link/proto_frames.h):
 *   - Pure C99, no OS/IDF headers, host-compilable. Header-only: all codec
 *     functions are static inline, no .c / no build-system integration needed
 *     on either side.
 *   - All multi-byte wire fields are EXPLICIT LITTLE-ENDIAN. Never cast a host
 *     struct onto wire bytes; use cam_frame_encode / cam_frame_parse.
 *   - One WebSocket binary frame carries exactly ONE complete
 *     header + JPEG payload (TX-side hard constraint, design G-2).
 *
 * Wire layout (160 bytes of JPEG may follow at offset 20):
 *   offset size field
 *   0      2    MAGIC        0xCA 0x56
 *   2      1    VERSION      0x01
 *   3      1    FLAGS        bit0 keyframe / bit1 event / bit2..7 reserved
 *   4      4    SEQ          u32, monotonic per gateway power-on
 *   8      4    TIMESTAMP_MS u32, gateway uptime_ms at capture (e2e diag only)
 *   12     2    WIDTH        u16, image width of the JPEG payload
 *   14     2    HEIGHT       u16
 *   16     4    JPEG_LEN     u32, payload byte count, 0 < len <= CAM_JPEG_LEN_MAX
 *   20     N    PAYLOAD      baseline JPEG (starts FFD8, ends FFD9)
 */
#ifndef CAM_FRAME_H
#define CAM_FRAME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- frame constants ------------------------------------------------------ */
#define CAM_MAGIC1          0xCAu
#define CAM_MAGIC2          0x56u
#define CAM_FRAME_VER       0x01u
#define CAM_HEADER_LEN      20u

/* field byte offsets */
#define CAM_OFF_MAGIC       0u
#define CAM_OFF_VERSION     2u
#define CAM_OFF_FLAGS       3u
#define CAM_OFF_SEQ         4u
#define CAM_OFF_TIMESTAMP   8u
#define CAM_OFF_WIDTH       12u
#define CAM_OFF_HEIGHT      14u
#define CAM_OFF_JPEG_LEN    16u
#define CAM_OFF_PAYLOAD     20u

/* FLAGS bits (reserved bits must be sent as 0, ignored on parse) */
#define CAM_FLAG_KEYFRAME   0x01u
#define CAM_FLAG_EVENT      0x02u

/* Protocol ceiling for one JPEG payload.  The receiver's buffer may be
 * smaller (CONFIG_SCR_CAM_MAX_JPEG_BYTES); anything above either limit is
 * dropped as CAM_RX_LEN_ERR. */
#define CAM_JPEG_LEN_MAX    65536u

/* Largest sane SEQ gap treated as real frame loss.  A larger forward jump
 * means the sender restarted (uptime/seq reset) -> resync, do not count drops.
 * Backward or duplicate SEQ is always a stale frame (CAM_SEQ_STALE). */
#define CAM_SEQ_GAP_MAX     512u

/* ---- parsed header (host byte order) ---------------------------------------*/
typedef struct
{
    uint8_t  version;               /* CAM_FRAME_VER after a successful parse  */
    uint8_t  flags;                 /* CAM_FLAG_* bitmask                      */
    uint32_t seq;
    uint32_t timestamp_ms;          /* sender uptime at capture; diagnostic    */
    uint16_t width;
    uint16_t height;
    uint32_t jpeg_len;              /* payload bytes following the header      */
} cam_frame_hdr_t;

/* ---- parse results ----------------------------------------------------------*/
typedef enum
{
    CAM_RX_OK = 0,          /* header valid; total == CAM_OFF_PAYLOAD + jpeg_len */
    CAM_RX_TOO_SHORT,       /* fewer than CAM_HEADER_LEN bytes available        */
    CAM_RX_MAGIC_ERR,       /* sync bytes mismatch                              */
    CAM_RX_VER_ERR,         /* unknown VERSION (peer firmware skew)             */
    CAM_RX_LEN_ERR,         /* jpeg_len == 0, > CAM_JPEG_LEN_MAX, or buffer
                               shorter than header + payload                    */
    CAM_RX_JPEG_ERR         /* header consistent but payload is not a JPEG
                               stream (missing FFD8 / FFD9)                     */
} cam_rx_ev_t;

/* ---- LE field helpers (same convention as proto_put/get) -------------------*/
static inline void cam_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static inline void cam_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint16_t cam_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t cam_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- encode (TX: s3-gateway camera_ws) -------------------------------------*/
/*
 * Serialize h into out (header only; caller memcpy's the JPEG right after).
 * Validates jpeg_len against the ceiling and cap.
 * Returns CAM_HEADER_LEN, or 0 on invalid argument.
 */
static inline size_t cam_frame_encode(const cam_frame_hdr_t *h,
                                      uint8_t *out, size_t cap)
{
    if (h == NULL || out == NULL || cap < CAM_HEADER_LEN) {
        return 0;
    }
    if (h->jpeg_len == 0u || h->jpeg_len > CAM_JPEG_LEN_MAX) {
        return 0;
    }
    if (h->width == 0u || h->height == 0u) {
        return 0;
    }
    out[0] = CAM_MAGIC1;
    out[1] = CAM_MAGIC2;
    out[CAM_OFF_VERSION] = CAM_FRAME_VER;
    out[CAM_OFF_FLAGS] = (uint8_t)(h->flags & (CAM_FLAG_KEYFRAME | CAM_FLAG_EVENT));
    cam_put_u32(&out[CAM_OFF_SEQ], h->seq);
    cam_put_u32(&out[CAM_OFF_TIMESTAMP], h->timestamp_ms);
    cam_put_u16(&out[CAM_OFF_WIDTH], h->width);
    cam_put_u16(&out[CAM_OFF_HEIGHT], h->height);
    cam_put_u32(&out[CAM_OFF_JPEG_LEN], h->jpeg_len);
    return CAM_HEADER_LEN;
}

/* Convenience: encode header for a payload of jpeg_len bytes.
 * Returns CAM_HEADER_LEN or 0. */
static inline size_t cam_frame_build(uint32_t seq, uint32_t timestamp_ms,
                                     uint16_t w, uint16_t hgt,
                                     uint32_t jpeg_len, uint8_t flags,
                                     uint8_t *out, size_t cap)
{
    cam_frame_hdr_t h;
    h.version = CAM_FRAME_VER;
    h.flags = flags;
    h.seq = seq;
    h.timestamp_ms = timestamp_ms;
    h.width = w;
    h.height = hgt;
    h.jpeg_len = jpeg_len;
    return cam_frame_encode(&h, out, cap);
}

/* ---- parse / validate (RX: smartcar_remote scr_cam) ------------------------*/
/*
 * buf/len must cover a WHOLE WS binary frame (header + payload).
 * On CAM_RX_OK, *out is filled and the payload starts at buf[CAM_OFF_PAYLOAD]
 * with out->jpeg_len bytes.  Every non-OK result is a whole-frame drop:
 * increment the matching counter (frame_err for MAGIC/VER/LEN, decode-side
 * accounting for TOO_SHORT/JPEG_ERR) and keep the last displayed frame.
 */
static inline cam_rx_ev_t cam_frame_parse(const uint8_t *buf, size_t len,
                                          cam_frame_hdr_t *out)
{
    uint32_t jpeg_len;

    if (buf == NULL || out == NULL || len < CAM_HEADER_LEN) {
        return CAM_RX_TOO_SHORT;
    }
    if (buf[CAM_OFF_MAGIC] != CAM_MAGIC1 || buf[CAM_OFF_MAGIC + 1u] != CAM_MAGIC2) {
        return CAM_RX_MAGIC_ERR;
    }
    if (buf[CAM_OFF_VERSION] != CAM_FRAME_VER) {
        return CAM_RX_VER_ERR;
    }

    jpeg_len = cam_get_u32(&buf[CAM_OFF_JPEG_LEN]);
    if (jpeg_len == 0u || jpeg_len > CAM_JPEG_LEN_MAX ||
        len < (size_t)CAM_OFF_PAYLOAD + jpeg_len) {
        return CAM_RX_LEN_ERR;
    }

    /* JPEG sanity: SOI at payload start, EOI at payload end.  WS frames are
     * exact (header + jpeg_len bytes), so a missing EOI means a truncated or
     * corrupted frame -> whole-frame drop. */
    if (buf[CAM_OFF_PAYLOAD] != 0xFFu || buf[CAM_OFF_PAYLOAD + 1u] != 0xD8u) {
        return CAM_RX_JPEG_ERR;
    }
    if (buf[CAM_OFF_PAYLOAD + jpeg_len - 2u] != 0xFFu ||
        buf[CAM_OFF_PAYLOAD + jpeg_len - 1u] != 0xD9u) {
        return CAM_RX_JPEG_ERR;
    }

    out->version = buf[CAM_OFF_VERSION];
    out->flags = buf[CAM_OFF_FLAGS];
    out->seq = cam_get_u32(&buf[CAM_OFF_SEQ]);
    out->timestamp_ms = cam_get_u32(&buf[CAM_OFF_TIMESTAMP]);
    out->width = cam_get_u16(&buf[CAM_OFF_WIDTH]);
    out->height = cam_get_u16(&buf[CAM_OFF_HEIGHT]);
    out->jpeg_len = jpeg_len;
    return CAM_RX_OK;
}

/* ---- sequence bookkeeping (RX drop statistics, design doc §4.1) ------------*/
typedef enum
{
    CAM_SEQ_FRESH = 0,   /* strictly newer; *dropped holds the gap-1 loss count  */
    CAM_SEQ_STALE,       /* duplicate or backward: late/reordered frame, ignore */
    CAM_SEQ_RESYNC       /* sender restarted: gap > CAM_SEQ_GAP_MAX            */
} cam_seq_st_t;

/*
 * Pure function: compare the parsed hdr->seq against the last accepted seq.
 * Returns the classification and, for CAM_SEQ_FRESH, the number of frames
 * lost between last and now (may be 0).  Caller stores *inout_last = seq on
 * FRESH and RESYNC, never on STALE.
 */
static inline cam_seq_st_t cam_seq_note(uint32_t last, uint32_t has_history,
                                        uint32_t now, uint32_t *dropped)
{
    uint32_t gap;

    if (dropped != NULL) {
        *dropped = 0u;
    }
    if (!has_history) {
        return CAM_SEQ_RESYNC;           /* first frame of the session */
    }
    if (now == last) {
        return CAM_SEQ_STALE;            /* duplicate delivery */
    }
    if (now < last) {
        /* seq restarted at 0 on sender reboot: a big backward jump resyncs,
         * a small one is treated as stale/reordered and ignored */
        return (last - now > CAM_SEQ_GAP_MAX) ? CAM_SEQ_RESYNC : CAM_SEQ_STALE;
    }
    gap = now - last;
    if (gap > CAM_SEQ_GAP_MAX) {
        return CAM_SEQ_RESYNC;           /* sender restarted far ahead */
    }
    if (dropped != NULL) {
        *dropped = gap - 1u;
    }
    return CAM_SEQ_FRESH;
}

#ifdef __cplusplus
}
#endif

#endif /* CAM_FRAME_H */
