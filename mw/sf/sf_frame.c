/*
 * sf_frame.c - SmartDrive LINK segment frame (SF) codec
 *
 * See sf_frame.h for the wire format and the hard rules.  This file must stay
 * free of target-specific includes so the TriCore build, the ESP-IDF build on
 * the C6 side and the host unit tests all compile it verbatim.
 */
#include "mw/sf/sf_frame.h"

#define SF_ST_MAGIC     0u
#define SF_ST_FIELD     1u      /* VER .. CID (bytes 1 .. 7)                 */
#define SF_ST_PAYLOAD   2u
#define SF_ST_CRC_HI    3u
#define SF_ST_CRC_LO    4u

static uint16_t SF_crcStep(uint16_t crc, uint8_t byte)
{
    int8_t bit;

    crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)byte << 8));
    for (bit = 0; bit < 8; bit++)
    {
        if ((crc & 0x8000u) != 0u)
        {
            crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
        }
        else
        {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t SF_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;

    if (data == NULL)
    {
        return 0u;
    }

    for (i = 0u; i < len; i++)
    {
        crc = SF_crcStep(crc, data[i]);
    }
    return crc;
}

uint16_t SF_wireSize(uint16_t payloadLen)
{
    uint16_t raw;

    if (payloadLen > SF_MAX_PAYLOAD)
    {
        return 0u;
    }
    raw = (uint16_t)(SF_OVERHEAD + payloadLen);
    return (uint16_t)((raw + (SF_SEG_ALIGN - 1u)) & (uint16_t)~(SF_SEG_ALIGN - 1u));
}

int16_t SF_build(uint8_t type, uint8_t seq, uint8_t flags, uint8_t cid,
                 const uint8_t *payload, uint16_t len,
                 uint8_t *out, uint16_t cap)
{
    uint16_t raw;
    uint16_t wire;
    uint16_t crc;
    uint16_t i;

    if ((out == NULL) || (len > SF_MAX_PAYLOAD) || ((payload == NULL) && (len > 0u)))
    {
        return -1;
    }

    raw  = (uint16_t)(SF_OVERHEAD + len);
    wire = SF_wireSize(len);
    if (cap < wire)
    {
        return -1;
    }

    out[0] = SF_MAGIC;
    out[1] = SF_VERSION;
    out[2] = type;
    out[3] = seq;
    out[4] = flags;
    SF_putU16(&out[5], len);
    out[7] = cid;
    for (i = 0u; i < len; i++)
    {
        out[SF_HEADER_LEN + i] = payload[i];
    }

    crc = SF_crc16(out, (uint16_t)(SF_HEADER_LEN + len));
    out[SF_HEADER_LEN + len]      = (uint8_t)(crc >> 8);   /* MSB byte first */
    out[SF_HEADER_LEN + len + 1u] = (uint8_t)(crc & 0xFFu);

    for (i = raw; i < wire; i++)
    {
        out[i] = 0x00u;
    }
    return (int16_t)wire;
}

void SF_parserInit(SF_Parser *p)
{
    if (p != NULL)
    {
        p->state       = SF_ST_MAGIC;
        p->idx         = 0u;
        p->need        = 0u;
        p->lastSeq     = 0u;
        p->haveLastSeq = 0u;
        p->lastByteMs  = 0u;
        p->stats.frames    = 0u;
        p->stats.crcErr    = 0u;
        p->stats.fmtErr    = 0u;
        p->stats.verErr    = 0u;
        p->stats.seqErr    = 0u;
        p->stats.timeouts  = 0u;
    }
}

uint8_t SF_seqOk(uint8_t seq, uint8_t lastSeq)
{
    /* Strict forward window: 1 <= (seq - lastSeq) <= SF_SEQ_WINDOW, mod 256. */
    uint8_t delta = (uint8_t)(seq - lastSeq);

    return (uint8_t)((delta >= 1u) && (delta <= SF_SEQ_WINDOW));
}

static void SF_reset(SF_Parser *p)
{
    p->state = SF_ST_MAGIC;
    p->idx   = 0u;
    p->need  = 0u;
}

/* A rejected start byte may itself be a MAGIC (0x5A 0x5A 0x01 ...); feed it
 * back by entering the field state instead of the magic state. */
static void SF_abort(SF_Parser *p, uint8_t byte)
{
    if (byte == SF_MAGIC)
    {
        p->buf[0] = byte;
        p->idx    = 1u;
        p->state  = SF_ST_FIELD;
    }
    else
    {
        SF_reset(p);
    }
}

SF_Event SF_parserFeed(SF_Parser *p, uint8_t byte, uint32_t nowMs, SF_Frame *frame)
{
    uint16_t crcRx;
    uint16_t crcCalc;
    uint16_t len;

    if ((p == NULL) || (frame == NULL))
    {
        return SF_EV_FMT_ERR;
    }

    switch (p->state)
    {
        case SF_ST_MAGIC:
            if (byte == SF_MAGIC)
            {
                p->buf[0]     = byte;
                p->idx        = 1u;
                p->lastByteMs = nowMs;
                p->state      = SF_ST_FIELD;
            }
            break;

        case SF_ST_FIELD:
            p->buf[p->idx++] = byte;
            p->lastByteMs    = nowMs;
            if (p->idx >= SF_HEADER_LEN)
            {
                if (p->buf[1] != SF_VERSION)
                {
                    p->stats.verErr++;
                    SF_abort(p, byte);
                    return SF_EV_VER_ERR;
                }
                len = SF_getU16(&p->buf[5]);
                if (len > SF_MAX_PAYLOAD)
                {
                    p->stats.fmtErr++;
                    SF_abort(p, byte);
                    return SF_EV_FMT_ERR;
                }
                p->need  = len;
                p->state = (len > 0u) ? SF_ST_PAYLOAD : SF_ST_CRC_HI;
            }
            break;

        case SF_ST_PAYLOAD:
            p->buf[p->idx++] = byte;
            p->lastByteMs    = nowMs;
            if (p->idx >= (uint16_t)(SF_HEADER_LEN + p->need))
            {
                p->state = SF_ST_CRC_HI;
            }
            break;

        case SF_ST_CRC_HI:
            p->buf[p->idx++] = byte;
            p->lastByteMs    = nowMs;
            p->state         = SF_ST_CRC_LO;
            break;

        case SF_ST_CRC_LO:
            p->buf[p->idx++] = byte;
            p->lastByteMs    = nowMs;
            len              = SF_getU16(&p->buf[5]);

            crcRx   = (uint16_t)((uint16_t)p->buf[SF_HEADER_LEN + len] << 8);
            crcRx   = (uint16_t)(crcRx | p->buf[SF_HEADER_LEN + len + 1u]);
            crcCalc = SF_crc16(p->buf, (uint16_t)(SF_HEADER_LEN + len));
            SF_reset(p);

            if (crcRx != crcCalc)
            {
                p->stats.crcErr++;
                return SF_EV_CRC_ERR;
            }
            if ((p->haveLastSeq != 0u) &&
                (SF_seqOk(p->buf[3], p->lastSeq) == 0u))
            {
                /* Replay / stale / out-of-window: drop without advancing. */
                p->stats.seqErr++;
                return SF_EV_SEQ_ERR;
            }

            p->lastSeq      = p->buf[3];
            p->haveLastSeq  = 1u;
            p->stats.frames++;

            frame->type    = p->buf[2];
            frame->seq     = p->buf[3];
            frame->flags   = p->buf[4];
            frame->cid     = p->buf[7];
            frame->len     = len;
            frame->payload = &p->buf[SF_HEADER_LEN];
            return SF_EV_FRAME;

        default:
            p->stats.fmtErr++;
            SF_reset(p);
            return SF_EV_FMT_ERR;
    }

    return SF_EV_NONE;
}

uint8_t SF_parserBusy(const SF_Parser *p)
{
    if (p == NULL)
    {
        return 0u;
    }
    return (uint8_t)(p->state != SF_ST_MAGIC);
}

SF_Event SF_parserTick(SF_Parser *p, uint32_t nowMs)
{
    if ((p == NULL) || (p->state == SF_ST_MAGIC))
    {
        return SF_EV_NONE;
    }

    /* Unsigned subtraction, so a wrapping 32-bit ms base stays correct. */
    if ((uint32_t)(nowMs - p->lastByteMs) < SF_RESIDUAL_TIMEOUT_MS)
    {
        return SF_EV_NONE;
    }

    p->stats.timeouts++;
    SF_reset(p);
    return SF_EV_TIMEOUT;
}
