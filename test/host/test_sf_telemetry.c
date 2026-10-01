/*
 * test_sf_telemetry.c - host tests for the 38 byte SF telemetry payload
 *
 * Why this file exists: the slave drops every TEL frame that is not exactly
 * CID_TELEMETRY with at least 38 bytes (esp32c6_car components/c6_link/link.c:
 * sf_to_v2), and the TC275 side used to send six. A six byte payload was not
 * "less telemetry", it was no telemetry at all - so the layout here is tested
 * against the C6's own decoder rather than against a copy of my expectations.
 *
 * Build and run (MSYS2/MinGW host, from the repo root):
 *   gcc -std=c99 -Wall -Wextra -Werror -O2 -I . \
 *       test/host/test_sf_telemetry.c mw/sf/sf_telemetry.c \
 *       mw/sf/sf_frame.c -o test/host/out/test_sf_telemetry.exe \
 *       && test/host/out/test_sf_telemetry.exe
 *
 * With the cross-check against the sibling esp32c6_car checkout (recommended, it is
 * the part that tests the contract and not just the encoder). proto_frames.c
 * compiles warning free but is third party here, hence -Werror is dropped:
 *   gcc -std=c99 -Wall -Wextra -O2 -DC6_CROSS_CHECK -I . \
 *       -I ../esp32c6_car/components/c6_proto -I ../esp32c6_car/components/c6_sf \
 *       test/host/test_sf_telemetry.c mw/sf/sf_telemetry.c \
 *       mw/sf/sf_frame.c ../esp32c6_car/components/c6_proto/proto_frames.c \
 *       -o test/host/out/test_sf_telemetry.exe
 */
#include <stdio.h>
#include <string.h>

#include "mw/sf/sf_frame.h"
#include "mw/sf/sf_telemetry.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond))                                                      \
        {                                                                 \
            g_failed++;                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        long _a = (long)(a), _b = (long)(b);                              \
        g_checks++;                                                       \
        if (_a != _b)                                                     \
        {                                                                 \
            g_failed++;                                                   \
            printf("FAIL %s:%d  %s == %s  (%ld vs %ld)\n",                \
                   __FILE__, __LINE__, #a, #b, _a, _b);                   \
        }                                                                 \
    } while (0)

#define SENTINEL 0xA5u

/* Compare a decoded struct against the reference field by field.
 *
 * Deliberately not a whole-struct memcmp: SF_Telemetry carries padding after
 * state, batteryPct and linkErrRate (the wire has no gaps, the C struct does),
 * so the bytes between fields are undefined and would fail on their own. The
 * wire layout is what test_golden_bytes pins down byte for byte; here we only
 * care that every named field came back. */
#define CHECK_FIELDS(actual, expect)                                          \
    do {                                                                      \
        CHECK_EQ((actual).seq, (expect).seq);                                 \
        CHECK_EQ((actual).uptimeMs, (expect).uptimeMs);                        \
        CHECK_EQ((actual).state, (expect).state);                              \
        CHECK_EQ((actual).faultCode, (expect).faultCode);                      \
        CHECK_EQ((actual).vTargetLeft, (expect).vTargetLeft);                  \
        CHECK_EQ((actual).vTargetRight, (expect).vTargetRight);                \
        CHECK_EQ((actual).vMeasLeft, (expect).vMeasLeft);                      \
        CHECK_EQ((actual).vMeasRight, (expect).vMeasRight);                    \
        CHECK_EQ((actual).batteryMv, (expect).batteryMv);                      \
        CHECK_EQ((actual).batteryPct, (expect).batteryPct);                    \
        CHECK_EQ((actual).odoSessionMm, (expect).odoSessionMm);                \
        CHECK_EQ((actual).odoTotalMm, (expect).odoTotalMm);                    \
        CHECK_EQ((actual).linkRttMs, (expect).linkRttMs);                      \
        CHECK_EQ((actual).linkErrRate, (expect).linkErrRate);                  \
        CHECK_EQ((actual).fwVer, (expect).fwVer);                              \
        CHECK_EQ((actual).hwRev, (expect).hwRev);                              \
    } while (0)

/* Pre-fill value for the offset test; chosen because it appears nowhere in
 * g_refBytes, so every one of the 38 offsets is genuinely overwritten. */
#define SENTINEL 0xA5u

/* The reference frame: every field a value that is unique in the buffer, so a
 * wrong offset shows up as a wrong byte rather than as an accident. */
static const SF_Telemetry g_ref = {
    0x11223344u,             /* seq              */
    0x55667788u,             /* uptimeMs         */
    0x07u,                   /* state            */
    0x0301u,                 /* faultCode        */
    -21,                     /* vTargetLeft      */
    22,                      /* vTargetRight     */
    -23,                     /* vMeasLeft        */
    24,                      /* vMeasRight       */
    0x0FA0u,                 /* batteryMv        */
    77u,                     /* batteryPct       */
    0xAABBCCDDu,             /* odoSessionMm     */
    0x09080706u,             /* odoTotalMm       */
    0x0102u,                 /* linkRttMs        */
    13u,                     /* linkErrRate      */
    0x00010203u,             /* fwVer            */
    0x04u                    /* hwRev            */
};

/* Byte for byte expected wire image. Little endian, unaligned on purpose - the
 * field offsets come from doc 22 SS5.5 and are not a struct layout. */
static const uint8_t g_refBytes[SF_TELEMETRY_LEN] = {
    /* 00 */ 0x44u, 0x33u, 0x22u, 0x11u,
    /* 04 */ 0x88u, 0x77u, 0x66u, 0x55u,
    /* 08 */ 0x07u,
    /* 09 */ 0x01u, 0x03u,
    /* 0B */ 0xEBu, 0xFFu,                    /* -21  */
    /* 0D */ 0x16u, 0x00u,                    /*  22  */
    /* 0F */ 0xE9u, 0xFFu,                    /* -23  */
    /* 11 */ 0x18u, 0x00u,                    /*  24  */
    /* 13 */ 0xA0u, 0x0Fu,                    /* 4000 */
    /* 15 */ 0x4Du,
    /* 16 */ 0xDDu, 0xCCu, 0xBBu, 0xAAu,
    /* 1A */ 0x06u, 0x07u, 0x08u, 0x09u,
    /* 1E */ 0x02u, 0x01u,
    /* 20 */ 0x0Du,
    /* 21 */ 0x03u, 0x02u, 0x01u, 0x00u,
    /* 25 */ 0x04u
};

static void test_golden_bytes(void)
{
    uint8_t buf[64];
    int16_t n;

    memset(buf, 0xAAu, sizeof(buf));
    n = SF_telemetryEncode(&g_ref, buf, SF_TELEMETRY_LEN);
    CHECK_EQ(n, SF_TELEMETRY_LEN);
    CHECK(memcmp(buf, g_refBytes, SF_TELEMETRY_LEN) == 0);

    /* The encoder must not write past the payload it was given. */
    CHECK_EQ(buf[SF_TELEMETRY_LEN], 0xAAu);

    /* A buffer one byte short is refused, not partially filled. */
    CHECK_EQ(SF_telemetryEncode(&g_ref, buf, SF_TELEMETRY_LEN - 1u), -1);
    CHECK_EQ(SF_telemetryEncode(NULL, buf, sizeof(buf)), -1);
    CHECK_EQ(SF_telemetryEncode(&g_ref, NULL, sizeof(buf)), -1);
}

static void test_round_trip(void)
{
    uint8_t      buf[SF_TELEMETRY_LEN];
    SF_Telemetry out;

    CHECK(SF_telemetryEncode(&g_ref, buf, sizeof(buf)) == (int16_t)SF_TELEMETRY_LEN);

    memset(&out, 0x5Cu, sizeof(out));
    CHECK_EQ(SF_telemetryDecode(buf, SF_TELEMETRY_LEN, &out), 1u);
    CHECK_FIELDS(out, g_ref);

    /* Short input is the exact failure the slave guards, so the decoder must
     * refuse it the same way rather than decode a partial frame. */
    CHECK_EQ(SF_telemetryDecode(buf, SF_TELEMETRY_LEN - 1u, &out), 0u);
    CHECK_EQ(SF_telemetryDecode(NULL, SF_TELEMETRY_LEN, &out), 0u);
    CHECK_EQ(SF_telemetryDecode(buf, SF_TELEMETRY_LEN, NULL), 0u);
}

static void test_field_offsets(void)
{
    uint8_t buf[SF_TELEMETRY_LEN];
    uint8_t i;
    uint8_t untouched = 0u;

    /* Encode over a sentinel so a byte the encoder forgets to write cannot pass
     * as a coincidentally matching value, then pin each documented offset
     * individually - an offset that moved by one names itself in the failure. */
    memset(buf, SENTINEL, sizeof(buf));
    CHECK(SF_telemetryEncode(&g_ref, buf, sizeof(buf)) == (int16_t)SF_TELEMETRY_LEN);
    for (i = 0u; i < SF_TELEMETRY_LEN; i++)
    {
        if (buf[i] == SENTINEL && g_refBytes[i] != SENTINEL)
        {
            untouched++;
        }
    }
    CHECK_EQ(untouched, 0u);

    CHECK_EQ(buf[0], g_refBytes[0]);
    CHECK_EQ(buf[4], g_refBytes[4]);
    CHECK_EQ(buf[8], g_refBytes[8]);
    CHECK_EQ(buf[9], g_refBytes[9]);
    CHECK_EQ(buf[11], g_refBytes[11]);
    CHECK_EQ(buf[13], g_refBytes[13]);
    CHECK_EQ(buf[15], g_refBytes[15]);
    CHECK_EQ(buf[17], g_refBytes[17]);
    CHECK_EQ(buf[19], g_refBytes[19]);
    CHECK_EQ(buf[21], g_refBytes[21]);
    CHECK_EQ(buf[22], g_refBytes[22]);
    CHECK_EQ(buf[26], g_refBytes[26]);
    CHECK_EQ(buf[30], g_refBytes[30]);
    CHECK_EQ(buf[32], g_refBytes[32]);
    CHECK_EQ(buf[33], g_refBytes[33]);
    CHECK_EQ(buf[37], g_refBytes[37]);
}

/* The whole outbound path: encode, wrap in an SF frame with padding, parse the
 * segment back byte by byte, and read the payload again. This is what the pump
 * does with one telemetry slot. */
static void test_frame_round_trip(void)
{
    uint8_t      seg[SF_MAX_PADDED];
    uint8_t      payload[SF_TELEMETRY_LEN];
    SF_Telemetry out;
    SF_Parser    parser;
    SF_Frame     frame;
    SF_Event     ev;
    uint16_t     n;
    uint16_t     i;
    uint16_t     got = 0u;

    CHECK(SF_telemetryEncode(&g_ref, payload, sizeof(payload)) == (int16_t)SF_TELEMETRY_LEN);
    n = (uint16_t)SF_build(SF_TYPE_TEL, 0x41u, 0u, SF_CID_TELEMETRY,
                           payload, SF_TELEMETRY_LEN, seg, sizeof(seg));
    /* 8 header + 38 payload + 2 CRC = 48, already a multiple of 4. */
    CHECK_EQ(n, 48u);

    SF_parserInit(&parser);
    memset(&frame, 0, sizeof(frame));
    for (i = 0u; i < n; i++)
    {
        ev = SF_parserFeed(&parser, seg[i], 1000u, &frame);
        if (ev == SF_EV_FRAME)
        {
            got++;
        }
        else
        {
            CHECK(ev == SF_EV_NONE);
        }
    }
    CHECK_EQ(got, 1u);
    CHECK_EQ(frame.type, SF_TYPE_TEL);
    CHECK_EQ(frame.cid, SF_CID_TELEMETRY);
    CHECK_EQ(frame.len, SF_TELEMETRY_LEN);

    CHECK_EQ(SF_telemetryDecode(frame.payload, frame.len, &out), 1u);
    CHECK_FIELDS(out, g_ref);
}

/* The regression itself: the payload the pump used to produce. */
static void test_short_payload_is_rejected(void)
{
    uint8_t      buf[64];
    SF_Telemetry out;

    memset(buf, 0x5Au, sizeof(buf));
    /* Six fields, the old six byte layout - must not decode. */
    CHECK_EQ(SF_telemetryDecode(buf, 6u, &out), 0u);
}

#ifdef C6_CROSS_CHECK
/* Compile the slave's own codec into the test and prove the two agree. This is
 * the only check in this file that is not self referential: it compares against
 * the code that will read these bytes. */
#include "proto_frames.h"

static void test_against_c6_decoder(void)
{
    uint8_t            buf[SF_TELEMETRY_LEN];
    proto_telemetry_t  c6;
    SF_Telemetry       mine;

    CHECK_EQ((unsigned long)PROTO_TELEMETRY_LEN, (unsigned long)SF_TELEMETRY_LEN);

    /* My encoder's output, read by the slave's decoder. */
    CHECK(SF_telemetryEncode(&g_ref, buf, sizeof(buf)) == (int16_t)SF_TELEMETRY_LEN);
    CHECK_EQ(proto_telemetry_decode(buf, SF_TELEMETRY_LEN, &c6), 0);

    CHECK_EQ(c6.seq, g_ref.seq);
    CHECK_EQ(c6.uptime_ms, g_ref.uptimeMs);
    CHECK_EQ(c6.state, g_ref.state);
    CHECK_EQ(c6.fault_code, g_ref.faultCode);
    CHECK_EQ(c6.v_target_l, g_ref.vTargetLeft);
    CHECK_EQ(c6.v_target_r, g_ref.vTargetRight);
    CHECK_EQ(c6.v_meas_l, g_ref.vMeasLeft);
    CHECK_EQ(c6.v_meas_r, g_ref.vMeasRight);
    CHECK_EQ(c6.battery_mv, g_ref.batteryMv);
    CHECK_EQ(c6.battery_pct, g_ref.batteryPct);
    CHECK_EQ(c6.odo_session_mm, g_ref.odoSessionMm);
    CHECK_EQ(c6.odo_total_mm, g_ref.odoTotalMm);
    CHECK_EQ(c6.link_rtt_ms, g_ref.linkRttMs);
    CHECK_EQ(c6.link_err_rate, g_ref.linkErrRate);
    CHECK_EQ(c6.fw_ver, g_ref.fwVer);
    CHECK_EQ(c6.hw_rev, g_ref.hwRev);

    /* And the other direction: the slave's encoder read by my decoder. */
    memset(&c6, 0, sizeof(c6));
    c6.seq            = 0xDEADBEEFu;
    c6.uptime_ms      = 123456789u;
    c6.state          = 0x0Au;
    c6.fault_code     = 0x1234u;
    c6.v_target_l     = -32000;
    c6.v_target_r     = 32000;
    c6.v_meas_l       = -1;
    c6.v_meas_r       = 1;
    c6.battery_mv     = 16800u;
    c6.battery_pct    = 99u;
    c6.odo_session_mm = 0xFFFFFFFFu;
    c6.odo_total_mm   = 1u;
    c6.link_rtt_ms    = 65535u;
    c6.link_err_rate  = 255u;
    c6.fw_ver         = 0x00FFFFFFu;
    c6.hw_rev         = 0xFEu;

    CHECK_EQ(proto_telemetry_encode(&c6, buf, sizeof(buf)),
             (size_t)SF_TELEMETRY_LEN);
    CHECK_EQ(SF_telemetryDecode(buf, SF_TELEMETRY_LEN, &mine), 1u);
    CHECK_EQ(mine.seq, c6.seq);
    CHECK_EQ(mine.uptimeMs, c6.uptime_ms);
    CHECK_EQ(mine.state, c6.state);
    CHECK_EQ(mine.faultCode, c6.fault_code);
    CHECK_EQ(mine.vTargetLeft, c6.v_target_l);
    CHECK_EQ(mine.vTargetRight, c6.v_target_r);
    CHECK_EQ(mine.vMeasLeft, c6.v_meas_l);
    CHECK_EQ(mine.vMeasRight, c6.v_meas_r);
    CHECK_EQ(mine.batteryMv, c6.battery_mv);
    CHECK_EQ(mine.batteryPct, c6.battery_pct);
    CHECK_EQ(mine.odoSessionMm, c6.odo_session_mm);
    CHECK_EQ(mine.odoTotalMm, c6.odo_total_mm);
    CHECK_EQ(mine.linkRttMs, c6.link_rtt_ms);
    CHECK_EQ(mine.linkErrRate, c6.link_err_rate);
    CHECK_EQ(mine.fwVer, c6.fw_ver);
    CHECK_EQ(mine.hwRev, c6.hw_rev);

    /* The slave's acceptance rule for a TEL frame, in one assertion: a payload
     * one byte short is not decodable at all. */
    CHECK_EQ(proto_telemetry_decode(buf, SF_TELEMETRY_LEN - 1u, &c6), -1);
}
#endif /* C6_CROSS_CHECK */

int main(void)
{
    test_golden_bytes();
    test_round_trip();
    test_field_offsets();
    test_frame_round_trip();
    test_short_payload_is_rejected();
#ifdef C6_CROSS_CHECK
    test_against_c6_decoder();
#endif

    printf("telemetry: %d checks, %d failures%s\n", g_checks, g_failed,
#ifdef C6_CROSS_CHECK
           " (C6 cross check on)"
#else
           " (C6 cross check OFF: rebuild with -DC6_CROSS_CHECK)"
#endif
           );
    return (g_failed != 0) ? 1 : 0;
}
