/*
 * sf_frame.h - SmartDrive LINK segment frame (SF) codec, TC275 side
 *
 * Wire truth source: doc/20-design/22-link-spi-design.md SS5 (SF frame), mirrored
 * into SDD SS6.1a. The ESP32-C6 side implements the same layout (c6_sf); any
 * change here must be written back to both documents and to that component.
 *
 * Hard rules (same contract as esp32c6_car components/c6_proto/proto_frames.h):
 *   - Pure C99: stdint/stddef/string only, no OS/iLLD/IDF headers, no dynamic
 *     memory. Compiles verbatim for TriCore, RISC-V and the host unit tests.
 *   - All multi-byte wire fields are EXPLICIT LITTLE-ENDIAN (TriCore is big
 *     endian, RISC-V little endian); never cast a struct onto wire bytes.
 *
 * Frame layout (offsets from frame start):
 *   0     MAGIC   0x5A
 *   1     VER     0x01
 *   2     TYPE    SF_TYPE_*
 *   3     SEQ     per-direction, strictly advancing
 *   4     FLAGS   SF_FLAG_*
 *   5..6  LEN     u16 LE, payload byte count, <= SF_MAX_PAYLOAD
 *   7     CID     channel / command id within TYPE
 *   8..   payload [LEN]
 *   +     CRC16   CRC16-CCITT-FALSE over the first 8+LEN bytes, MSB byte first
 *   +     pad     0x00 up to a multiple of SF_SEG_ALIGN (slave RX constraint)
 *
 * One SPI data segment may carry several frames back to back; frame borders are
 * derived from LEN, and the trailing pad bytes are skipped by resynchronising on
 * MAGIC (a pad is never 0x5A, so a byte-oriented parser stays correct).
 */
#ifndef SF_FRAME_H
#define SF_FRAME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- wire constants ------------------------------------------------------- */
#define SF_MAGIC              0x5Au
#define SF_VERSION            0x01u
#define SF_HEADER_LEN         8u                   /* MAGIC .. CID                 */
#define SF_CRC_LEN            2u
#define SF_OVERHEAD           (SF_HEADER_LEN + SF_CRC_LEN)   /* 10, excl. pad      */
#define SF_MAX_PAYLOAD        248u                 /* LEN field, doc 22 SS5.1      */
#define SF_MAX_FRAME          (SF_OVERHEAD + SF_MAX_PAYLOAD) /* 258, excl. pad     */
#define SF_MAX_PADDED         (SF_MAX_FRAME + 3u)  /* 261, worst case aligned size */
#define SF_SEG_ALIGN          4u                   /* spi_slave_hd RX: mult. of 4  */

/* FLAGS (doc 22 SS5.1; values match the slave's c6_sf/sf_frame.h exactly).
 * There is no ACK flag: an acknowledgement is a frame of SF_TYPE_ACK, so bit1
 * belongs to the segmentation group. V1.0 sends unfragmented, and bit2..7 are
 * reserved and must go out as 0 - the slave copies FLAGS verbatim into the CRC,
 * so a locally invented bit would only ever cost us a CRC/format mismatch. */
#define SF_FLAG_FRAG          0x01u                /* continuation follows     */
#define SF_FLAG_FRAG_END      0x02u                /* last fragment of a frame */

/* TYPE channels (doc 22 SS5.2) */
#define SF_TYPE_CMD           0x01u                /* C6 -> TC275 command          */
#define SF_TYPE_TEL           0x02u                /* TC275 -> C6 telemetry        */
#define SF_TYPE_ACK           0x03u                /* both ways                    */
#define SF_TYPE_HBT           0x04u                /* both ways, carries RTT probe */
#define SF_TYPE_EVT           0x05u                /* TC275 -> C6 error/state      */
#define SF_TYPE_OTA_DATA      0x06u
#define SF_TYPE_OTA_CTRL      0x07u
#define SF_TYPE_DBG           0x08u
#define SF_TYPE_VND           0x0Fu

/* CID values inside a TYPE (doc 22 SS5.2). The payload shapes below are what the
 * C6 side actually puts on the wire (esp32c6_car components/c6_link/link.c:v2_to_sf),
 * and they are NOT uniform: only the three "op prefixed" channels carry a
 * command byte, the other two are the v2 payload verbatim. A receiver that
 * assumes payload[0] is always a command executes a configuration key as one.
 *   DRIVE {u8 op, i16 v, i16 w}   op is a v2 command code (doc 21 SS6.2); v/w are
 *                                 only filled for op 0x50 DRIVE and 0x10
 *                                 SET_SPEED, every other op sends 0/0
 *   DIAG  {u8 op, ...}            op 0x53 DIAG / 0x42 LINK_STATE
 *   DPT   {u8 op, ...}            op 0x70..0x79
 *   CFG   v2 0x52 payload as-is   {u8 op, k, v...} - op already stripped
 *   PAIR  v2 0x51 payload as-is   token[16] - op already stripped                          */
#define SF_CID_DRIVE          0x01u                /* TYPE_CMD: driving            */
#define SF_CID_CFG            0x02u
#define SF_CID_DIAG           0x03u
#define SF_CID_DPT            0x04u                /* production test              */
#define SF_CID_PAIR           0x05u
#define SF_CID_TELEMETRY      0x10u                /* TYPE_TEL, 38 B (sf_telemetry.h) */
/* TYPE_EVT: 0x20 carries {u16 errcode,...}; 0x21 carries {u8 kind,...} where
 * kind 1 is the pairing reply and the rest is a v2 0x51 payload verbatim. The
 * C6 side decodes exactly that (c6_link/link.c:sf_to_v2). */
#define SF_CID_ERROR          0x20u                /* TYPE_EVT                     */
#define SF_CID_STATE          0x21u                /* TYPE_EVT                     */
/* DPT bench events, TC275 -> C6 (doc 34 SS3.1 / SS9.1). Payload shapes are
 * built by mw/calib/calib_record.c; C6 tunnels them unchanged through its SF
 * DIAG bridge and turns them into UI events in c6_bridge/bridge.c. */
#define SF_CID_DPT_RESULT     0x22u                /* TYPE_EVT, 23 B calib result  */
#define SF_CID_DPT_REC        0x23u                /* TYPE_EVT, 15 B record echo   */
#define SF_EVT_KIND_PAIR_REPLY 0x01u               /* TYPE_EVT / CID_STATE byte 0  */
#define SF_CID_OTA_BEGIN      0x30u                /* TYPE_OTA_DATA {u32 total, u32 crc32} */
#define SF_CID_OTA_CHUNK      0x31u                /* TYPE_OTA_DATA {u16 idx, data<=240}   */
#define SF_CID_OTA_ACK        0x32u                /* TYPE_OTA_CTRL {u16 idx, u8 result}   */
#define SF_CID_OTA_STATUS     0x33u                /* TYPE_OTA_CTRL {u8 state, u8 pct}     */
#define SF_CID_OTA_SWAP       0x34u                /* TYPE_OTA_CTRL, empty payload         */
#define SF_CID_OTA_ABORT      0x35u                /* TYPE_OTA_DATA: drop half written slot*/
#define SF_OTA_CHUNK_MAX      240u                 /* doc 22 SS5.5: 62 B -> 240 B          */
#define SF_CID_LOG            0x40u                /* TYPE_DBG                     */

/* E2E / failure handling (doc 22 SS5.3, SS5.4) */
#define SF_SEQ_WINDOW         32u                  /* accept 1 <= seq-last <= 32   */
#define SF_SEQ_RELOCK_RUN     8u                   /* this many consecutive out-of-
                                                    * window rejects drop the window:
                                                    * the sender restarted and its
                                                    * counter will not come back into
                                                    * 1..32 on its own (up to 224
                                                    * frames would be rejected until
                                                    * the u8 wraps) - the next frame
                                                    * re-locks instead            */
#define SF_RESIDUAL_TIMEOUT_MS 4u                  /* partial frame older than this
                                                    * is dropped; the owner feeds
                                                    * SF_parserTick() with its ms
                                                    * time base                  */

/* ---- parser result codes --------------------------------------------------- */
typedef enum
{
    SF_EV_NONE = 0,             /* byte taken, nothing complete yet            */
    SF_EV_FRAME,                /* frame accepted: CRC and SEQ window both ok  */
    SF_EV_CRC_ERR,              /* dropped: CRC mismatch                       */
    SF_EV_FMT_ERR,              /* dropped: bad LEN or truncated segment       */
    SF_EV_VER_ERR,              /* dropped: unknown VER                        */
    SF_EV_SEQ_ERR,              /* dropped: SEQ outside the forward window     */
    SF_EV_TIMEOUT               /* a partial frame was discarded by Tick       */
} SF_Event;

/* A decoded frame. payload points INTO the parser buffer and is only valid
 * until the next accepted frame, so consumers must copy what they keep. */
typedef struct
{
    uint8_t        type;
    uint8_t        seq;
    uint8_t        flags;
    uint8_t        cid;
    uint16_t       len;
    const uint8_t *payload;
} SF_Frame;

typedef struct
{
    uint32_t frames;            /* accepted frames                             */
    uint32_t crcErr;
    uint32_t fmtErr;
    uint32_t verErr;
    uint32_t seqErr;
    uint32_t timeouts;
} SF_Stats;

/* One parser instance serves ONE direction (it carries that direction's SEQ
 * state); the link uses two of them. */
typedef struct
{
    uint8_t   state;
    uint8_t   buf[SF_MAX_FRAME];
    uint16_t  idx;              /* bytes collected in buf                      */
    uint16_t  need;             /* payload bytes still expected                */
    uint8_t   lastSeq;
    uint8_t   haveLastSeq;      /* 0 until the first frame of this direction   */
    uint8_t   seqRejRun;        /* consecutive out-of-window rejects; at
                                 * SF_SEQ_RELOCK_RUN the window is dropped    */
    uint32_t  lastByteMs;       /* only meaningful while a frame is in progress*/
    SF_Stats  stats;
} SF_Parser;

/* ---- CRC ------------------------------------------------------------------- */
/* CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF, MSB first, no reflection,
 * no final xor.  check("123456789") == 0x29B1.  Same variant as proto v2. */
uint16_t SF_crc16(const uint8_t *data, uint16_t len);

/* ---- encode ---------------------------------------------------------------- */
/* Wire size of a complete frame for a given payload length, padding included.
 * Returns 0 when len exceeds SF_MAX_PAYLOAD. */
uint16_t SF_wireSize(uint16_t payloadLen);

/* Build one frame (padding included) into out.
 * Returns the number of bytes written, or -1 on bad arguments /
 * out of space / NULL payload with len > 0. */
int16_t SF_build(uint8_t type, uint8_t seq, uint8_t flags, uint8_t cid,
                 const uint8_t *payload, uint16_t len,
                 uint8_t *out, uint16_t cap);

/* ---- decode ---------------------------------------------------------------- */
void SF_parserInit(SF_Parser *p);

/* Feed one byte of a received segment. nowMs is the caller's ms time base and
 * only stamps the partial-frame timer.  On SF_EV_FRAME, *frame is filled. */
SF_Event SF_parserFeed(SF_Parser *p, uint8_t byte, uint32_t nowMs, SF_Frame *frame);

/* Expire a partial frame that has not seen a continuation byte for
 * SF_RESIDUAL_TIMEOUT_MS. Call from the link pump, not from an ISR.
 * Returns SF_EV_TIMEOUT when a residual frame was dropped, else SF_EV_NONE. */
SF_Event SF_parserTick(SF_Parser *p, uint32_t nowMs);

/* 1 while a partial frame is buffered, i.e. the pump still owes the parser
 * continuation bytes (or a Tick that discards them). */
uint8_t SF_parserBusy(const SF_Parser *p);

/* 1 when seq is inside the strict forward window after lastSeq. */
uint8_t SF_seqOk(uint8_t seq, uint8_t lastSeq);

/* ---- explicit little-endian field helpers (shared with the C6 side) --------- */
static inline void SF_putU16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static inline void SF_putU32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint16_t SF_getU16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t SF_getU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef __cplusplus
}
#endif

#endif /* SF_FRAME_H */
