/*
 * test_ota_frames.c - G-OTA-2: SF OTA frame codec host tests
 *
 * Asserts the exact byte layout of the OTA channels against the values the
 * C6 side produces (esp32c6_car components/c6_link/link.c v2_to_sf /
 * link_send_ota_chunk and components/c6_proto/proto_frames.h):
 *   BEGIN  TYPE 0x06 CID 0x30 payload {u32 total LE, u32 crc32 LE}
 *   CHUNK  TYPE 0x06 CID 0x31 payload {u16 idx LE, data <= 240}
 *   ACK    TYPE 0x07 CID 0x32 payload {u16 idx LE, u8 result}
 *   STATUS TYPE 0x07 CID 0x33 payload {u8 state, u8 pct}
 *   SWAP   TYPE 0x07 CID 0x34 empty
 *   ABORT  TYPE 0x06 CID 0x35 empty
 * plus the frame envelope (magic/ver/len/CRC16-CCITT-FALSE) and a decode
 * round-trip through the byte-oriented parser.
 */
#include "test.h"

#include "mw/sf/sf_frame.h"
#include "mw/ota/ota_rx.h"
#include "test_vectors.h"

/* decode helper: run a built frame through the parser (a whole frame in one
 * segment is the normal shape for these short control frames) */
static SF_Frame decode(const uint8_t *buf, uint16_t len)
{
    static SF_Parser p;    /* outlives the return: payload points into it */
    SF_Frame f;
    uint16_t i;

    SF_parserInit(&p);
    for (i = 0u; i < len; i++)
    {
        if (SF_parserFeed(&p, buf[i], 1000u + i, &f) == SF_EV_FRAME)
        {
            return f;
        }
    }
    memset(&f, 0, sizeof(f));
    f.type = 0xFFu;                     /* marker: nothing decoded */
    return f;
}

int main(void)
{
    uint8_t buf[SF_MAX_FRAME];
    int16_t n;
    SF_Frame f;

    /* CRC16 sanity against the reference vector */
    T_CASE("crc16 reference");
    T_CHECK_EQ_I(SF_crc16((const uint8_t *)"123456789", 9u), 0x29B1);

    /* BEGIN: exactly the 8-byte body C6 forwards from v2 0x60 */
    T_CASE("OTA_BEGIN layout");
    {
        uint8_t body[8];
        uint32_t total = 0x12345678u;
        uint32_t crc = 0xDEADBEEFu;

        SF_putU32(&body[0], total);
        SF_putU32(&body[4], crc);
        n = SF_build(SF_TYPE_OTA_DATA, 7u, 0u, SF_CID_OTA_BEGIN,
                     body, 8u, buf, sizeof(buf));
        T_CHECK(n > 0);
        T_CHECK_EQ_I(buf[0], 0x5A);                 /* MAGIC               */
        T_CHECK_EQ_I(buf[1], 0x01);                 /* VER                 */
        T_CHECK_EQ_I(buf[2], SF_TYPE_OTA_DATA);     /* 0x06                */
        T_CHECK_EQ_I(buf[3], 7u);                   /* SEQ                 */
        T_CHECK_EQ_I(buf[4], 0u);                   /* FLAGS               */
        T_CHECK_EQ_I(SF_getU16(&buf[5]), 8u);       /* LEN                 */
        T_CHECK_EQ_I(buf[7], SF_CID_OTA_BEGIN);     /* 0x30                */
        T_CHECK_EQ_I(SF_getU32(&buf[8]), 0x12345678u);
        T_CHECK_EQ_I(SF_getU32(&buf[12]), 0xDEADBEEFu);
        f = decode(buf, (uint16_t)n);
        T_CHECK_EQ_I(f.type, SF_TYPE_OTA_DATA);
        T_CHECK_EQ_I(f.cid, SF_CID_OTA_BEGIN);
        T_CHECK_EQ_I(f.len, 8u);
        T_CHECK(f.payload == &buf[8] || f.payload != 0);
    }

    /* CHUNK: 2-byte LE idx then data, max 240 data bytes */
    T_CASE("OTA_CHUNK layout");
    {
        uint8_t body[2 + 240];
        uint16_t i;

        SF_putU16(&body[0], 0x0100u);              /* idx = 256              */
        for (i = 0u; i < 240u; i++)
        {
            body[2 + i] = (uint8_t)i;
        }
        n = SF_build(SF_TYPE_OTA_DATA, 8u, 0u, SF_CID_OTA_CHUNK,
                     body, 2u + 240u, buf, sizeof(buf));
        T_CHECK(n > 0);
        T_CHECK_EQ_I(buf[2], SF_TYPE_OTA_DATA);
        T_CHECK_EQ_I(buf[7], SF_CID_OTA_CHUNK);     /* 0x31                */
        T_CHECK_EQ_I(SF_getU16(&buf[5]), 242u);
        f = decode(buf, (uint16_t)n);
        T_CHECK_EQ_I(f.cid, SF_CID_OTA_CHUNK);
        T_CHECK_EQ_I(SF_getU16(f.payload), 0x0100u);/* idx LE in payload[0] */
        T_CHECK(f.payload != 0);
        T_CHECK_EQ_I(f.payload[2], 0u);
        T_CHECK_EQ_I(f.payload[2 + 239], 239u);
    }

    /* ACK: mirrors c6 link.c sf_to_v2's "3 bytes, >= 3" expectation */
    T_CASE("OTA_ACK layout");
    {
        uint8_t body[3];

        SF_putU16(&body[0], 0x00FFu);
        body[2] = OTARX_ACK_OK;
        n = SF_build(SF_TYPE_OTA_CTRL, 9u, 0u, SF_CID_OTA_ACK,
                     body, 3u, buf, sizeof(buf));
        T_CHECK(n > 0);
        T_CHECK_EQ_I(buf[2], SF_TYPE_OTA_CTRL);     /* 0x07                */
        T_CHECK_EQ_I(buf[7], SF_CID_OTA_ACK);       /* 0x32                */
        f = decode(buf, (uint16_t)n);
        T_CHECK_EQ_I(f.len, 3u);
        T_CHECK_EQ_I(SF_getU16(f.payload), 255u);
        T_CHECK_EQ_I(f.payload[2], 0u);
    }

    /* STATUS */
    T_CASE("OTA_STATUS layout");
    {
        uint8_t body[2];

        body[0] = OTARX_STATUS_DONE;
        body[1] = 100u;
        n = SF_build(SF_TYPE_OTA_CTRL, 10u, 0u, SF_CID_OTA_STATUS,
                     body, 2u, buf, sizeof(buf));
        T_CHECK(n > 0);
        f = decode(buf, (uint16_t)n);
        T_CHECK_EQ_I(f.cid, SF_CID_OTA_STATUS);     /* 0x33                */
        T_CHECK_EQ_I(f.len, 2u);
        T_CHECK_EQ_I(f.payload[0], OTARX_STATUS_DONE);
        T_CHECK_EQ_I(f.payload[1], 100u);
    }

    /* SWAP: empty payload, and a corrupt CRC is dropped by the parser */
    T_CASE("OTA_SWAP layout + CRC gate");
    {
        n = SF_build(SF_TYPE_OTA_CTRL, 11u, 0u, SF_CID_OTA_SWAP,
                     0, 0u, buf, sizeof(buf));
        T_CHECK(n > 0);
        f = decode(buf, (uint16_t)n);
        T_CHECK_EQ_I(f.cid, SF_CID_OTA_SWAP);       /* 0x34                */
        T_CHECK_EQ_I(f.len, 0u);

        buf[8] ^= 0x01u;                            /* any payload bit...  */
        buf[8 - 8u] = buf[0];
        {
            uint16_t wire = (uint16_t)n;

            buf[wire - 2u] ^= 0xFFu;                /* ...or the CRC       */
        }
        f = decode(buf, (uint16_t)n);
        T_CHECK_EQ_I(f.type, 0xFFu);                /* nothing accepted    */
    }

    /* ABORT: OTA_DATA typed with empty body (0x35) */
    T_CASE("OTA_ABORT layout");
    {
        n = SF_build(SF_TYPE_OTA_DATA, 12u, 0u, SF_CID_OTA_ABORT,
                     0, 0u, buf, sizeof(buf));
        T_CHECK(n > 0);
        f = decode(buf, (uint16_t)n);
        T_CHECK_EQ_I(f.type, SF_TYPE_OTA_DATA);
        T_CHECK_EQ_I(f.cid, SF_CID_OTA_ABORT);      /* 0x35                */
        T_CHECK_EQ_I(f.len, 0u);
    }

    T_RESULT("test_ota_frames");
}
