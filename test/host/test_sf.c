/*
 * test_sf.c - host unit + fuzz tests for the SF link frame codec
 *
 * Gate G2 of doc/20-design/22-link-spi-design.md SS8:
 *   - a random byte storm must not crash, hang or write out of bounds
 *   - encode/decode round trip with zero errors
 *   - 4-byte padding, multi-frame segments, residual (FRAG) timeout
 *
 * Build & run (MSYS2/MinGW host):
 *   gcc -std=c99 -Wall -Wextra -Werror -O2 -I . \
 *       test/host/test_sf.c mw/sf/sf_frame.c -o test/host/test_sf.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mw/sf/sf_frame.h"

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

static uint8_t seg[512];
static uint8_t payload[SF_MAX_PAYLOAD];
static uint8_t frame[SF_MAX_PADDED];
static uint8_t got[SF_MAX_PAYLOAD];

static void feed_all(SF_Parser *p, const uint8_t *b, uint16_t n, uint32_t nowMs)
{
    SF_Frame f;
    uint16_t i;

    for (i = 0u; i < n; i++)
    {
        (void)SF_parserFeed(p, b[i], nowMs, &f);
    }
}

/* ---------------- CRC known answer ---------------- */
static void test_crc_kat(void)
{
    /* CRC16-CCITT-FALSE check value for "123456789" is 0x29B1. */
    CHECK_EQ(SF_crc16((const uint8_t *)"123456789", 9), 0x29B1);
    CHECK_EQ(SF_crc16(NULL, 9), 0);
    CHECK_EQ(SF_crc16((const uint8_t *)"", 0), 0xFFFF);
}

/* ---------------- segment sizing / padding ---------------- */
static void test_wire_size(void)
{
    /* raw = 10 + len, aligned up to a multiple of 4 */
    CHECK_EQ(SF_wireSize(0), 12);
    CHECK_EQ(SF_wireSize(1), 12);
    CHECK_EQ(SF_wireSize(2), 12);
    CHECK_EQ(SF_wireSize(3), 16);
    CHECK_EQ(SF_wireSize(248), 260);
    /* the largest frame must still fit the declared worst-case buffer */
    CHECK(SF_wireSize(SF_MAX_PAYLOAD) <= SF_MAX_PADDED);
    CHECK_EQ(SF_wireSize(SF_MAX_PAYLOAD + 1u), 0);
    CHECK_EQ(SF_MAX_FRAME, 258);
    CHECK_EQ(SF_OVERHEAD, 10);
}

/* ---------------- layout of a built frame ---------------- */
static void test_layout(void)
{
    int16_t n;
    uint16_t crc;
    uint16_t i;

    for (i = 0u; i < 8u; i++)
    {
        payload[i] = (uint8_t)(0x10u + i);
    }
    n = SF_build(SF_TYPE_CMD, 7, SF_FLAG_FRAG, SF_CID_DRIVE, payload, 8, seg, sizeof(seg));
    CHECK_EQ(n, 20);                          /* raw 18 -> 20 */
    CHECK_EQ(seg[0], SF_MAGIC);
    CHECK_EQ(seg[1], SF_VERSION);
    CHECK_EQ(seg[2], SF_TYPE_CMD);
    CHECK_EQ(seg[3], 7);
    CHECK_EQ(seg[4], SF_FLAG_FRAG);
    CHECK_EQ(SF_getU16(&seg[5]), 8);          /* LEN little-endian */
    CHECK_EQ(seg[7], SF_CID_DRIVE);
    CHECK(memcmp(&seg[8], payload, 8) == 0);
    crc = SF_crc16(seg, (uint16_t)(SF_HEADER_LEN + 8));
    CHECK_EQ(seg[16], (uint8_t)(crc >> 8));   /* MSB byte first */
    CHECK_EQ(seg[17], (uint8_t)(crc & 0xFF));
    CHECK_EQ(seg[18], 0);                     /* pad */
    CHECK_EQ(seg[19], 0);
}

/* ---------------- build error paths ---------------- */
static void test_build_errors(void)
{
    CHECK_EQ(SF_build(SF_TYPE_CMD, 0, 0, SF_CID_DRIVE, payload, 4, NULL, 16), -1);
    /* LEN 4 -> raw 14 -> padded wire 16, so cap 15 must be refused */
    CHECK_EQ(SF_build(SF_TYPE_CMD, 0, 0, SF_CID_DRIVE, payload, 4, seg, 15), -1);
    CHECK_EQ(SF_build(SF_TYPE_CMD, 0, 0, SF_CID_DRIVE, payload, 4, seg, 16), 16);
    CHECK_EQ(SF_build(SF_TYPE_CMD, 0, 0, SF_CID_DRIVE, payload,
                      SF_MAX_PAYLOAD + 1u, seg, sizeof(seg)), -1);
    CHECK_EQ(SF_build(SF_TYPE_CMD, 0, 0, SF_CID_DRIVE, NULL, 4, seg, sizeof(seg)), -1);
    CHECK_EQ(SF_build(SF_TYPE_CMD, 0, 0, SF_CID_DRIVE, NULL, 0, seg, sizeof(seg)), 12);
}

/* ---------------- round trip over every legal payload size ---------------- */
static void test_round_trip(void)
{
    uint16_t len;

    for (len = 0u; len <= SF_MAX_PAYLOAD; len++)
    {
        SF_Parser p;
        SF_Frame f;
        int16_t n;
        SF_Event ev = SF_EV_NONE;
        uint16_t i;

        for (i = 0u; i < len; i++)
        {
            payload[i] = (uint8_t)(i * 31u + 7u);
        }
        SF_parserInit(&p);
        CHECK_EQ(SF_parserBusy(&p), 0);
        n = SF_build(SF_TYPE_TEL, 1, 0, SF_CID_TELEMETRY, len ? payload : NULL,
                     len, seg, sizeof(seg));
        CHECK(n == (int16_t)SF_wireSize(len));

        for (i = 0u; i < (uint16_t)n; i++)
        {
            ev = SF_parserFeed(&p, seg[i], 100u + i, &f);
            if (ev != SF_EV_NONE)
            {
                break;
            }
        }
        CHECK_EQ(ev, SF_EV_FRAME);
        CHECK_EQ(f.type, SF_TYPE_TEL);
        CHECK_EQ(f.cid, SF_CID_TELEMETRY);
        CHECK_EQ(f.len, len);
        (void)memcpy(got, f.payload, len);
        CHECK(memcmp(got, payload, len) == 0);
        CHECK_EQ(p.stats.frames, 1);
        CHECK_EQ(p.stats.crcErr, 0);
        CHECK_EQ(p.stats.fmtErr, 0);
        /* a complete frame leaves no residue for the pump to keep feeding */
        CHECK_EQ(SF_parserBusy(&p), 0);
    }
}

/* ---------------- multiple frames inside one DMA segment ---------------- */
static void test_multi_frame_segment(void)
{
    SF_Parser p;
    SF_Frame f;
    uint16_t off = 0;
    int16_t n;
    int seen = 0;
    uint8_t t;
    uint16_t i;

    SF_parserInit(&p);
    for (t = 0u; t < 3u; t++)
    {
        memset(payload, (int)(0x40u + t), 6);
        n = SF_build((uint8_t)(SF_TYPE_CMD + t), t, 0, SF_CID_DRIVE, payload, 6,
                     &seg[off], (uint16_t)(sizeof(seg) - off));
        CHECK(n > 0);
        off = (uint16_t)(off + n);
    }
    /* a final frame with the largest payload crosses padding boundaries */
    for (i = 0u; i < SF_MAX_PAYLOAD; i++)
    {
        payload[i] = (uint8_t)i;
    }
    n = SF_build(SF_TYPE_DBG, 3, 0, SF_CID_LOG, payload, SF_MAX_PAYLOAD, &seg[off],
                 (uint16_t)(sizeof(seg) - off));
    CHECK_EQ(n, 260);
    off = (uint16_t)(off + n);

    for (i = 0u; i < off; i++)
    {
        if (SF_parserFeed(&p, seg[i], 10u + i, &f) == SF_EV_FRAME)
        {
            if (seen < 3)
            {
                CHECK_EQ(f.type, (uint8_t)(SF_TYPE_CMD + seen));
                CHECK_EQ(f.seq, (uint8_t)seen);
                CHECK_EQ(f.len, 6);
                (void)memcpy(got, f.payload, f.len);
                for (uint16_t k = 0u; k < 6u; k++)
                {
                    CHECK_EQ(got[k], (uint8_t)(0x40u + seen));
                }
            }
            else
            {
                CHECK_EQ(f.type, SF_TYPE_DBG);
                CHECK_EQ(f.len, SF_MAX_PAYLOAD);
                (void)memcpy(got, f.payload, f.len);
                CHECK(memcmp(got, payload, SF_MAX_PAYLOAD) == 0);
            }
            seen++;
        }
    }
    CHECK_EQ(seen, 4);
    CHECK_EQ(p.stats.crcErr, 0);
    CHECK_EQ(p.stats.fmtErr, 0);
}

/* ---------------- junk, stray magics and recovery ---------------- */
static void test_resync(void)
{
    SF_Parser p;
    SF_Frame f;
    int16_t n;
    uint16_t off = 0;
    SF_Event ev = SF_EV_NONE;
    uint16_t i;

    /* two false starts: a bad VER (last byte is itself a MAGIC, so the parser
     * must re-enter on it) and an oversized LEN */
    static const uint8_t junk[] = {
        0x00, 0xFF,                             /* dropped as noise         */
        0x5A, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x5A,  /* VER_ERR, re-enter */
        0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33          /* VER_ERR again    */
    };
    static const uint8_t bigLen[] = {0x5A, 0x01, 0x01, 0x00, 0x00, 0xF9, 0x00, 0x00};

    memset(payload, 0x5A, 4);                  /* payload made of MAGIC bytes */
    n = SF_build(SF_TYPE_CMD, 0, 0, SF_CID_DRIVE, payload, 4, frame, sizeof(frame));
    CHECK_EQ(n, 16);

    SF_parserInit(&p);
    memcpy(&seg[off], junk, sizeof(junk));
    off = (uint16_t)(off + sizeof(junk));
    memcpy(&seg[off], bigLen, sizeof(bigLen));
    off = (uint16_t)(off + sizeof(bigLen));
    memcpy(&seg[off], frame, (size_t)n);       /* the real frame             */
    off = (uint16_t)(off + n);

    for (i = 0u; i < off; i++)
    {
        ev = SF_parserFeed(&p, seg[i], 50u + i, &f);
        if (ev == SF_EV_FRAME)
        {
            break;
        }
    }
    CHECK_EQ(ev, SF_EV_FRAME);
    CHECK_EQ(f.len, 4);
    (void)memcpy(got, f.payload, f.len);
    CHECK(memcmp(got, payload, 4) == 0);       /* payload survived the junk  */
    CHECK_EQ(p.stats.verErr, 2);
    CHECK_EQ(p.stats.fmtErr, 1);
    CHECK_EQ(p.stats.frames, 1);

    /* a fresh frame still parses after a clean cut + expiry */
    n = SF_build(SF_TYPE_CMD, 1, 0, SF_CID_DRIVE, payload, 4, frame, sizeof(frame));
    feed_all(&p, frame, (uint16_t)n, 200u);
    CHECK_EQ(p.stats.frames, 2);
    CHECK_EQ(p.lastSeq, 1);
}

static void test_crc_corruption(void)
{
    SF_Parser p;
    SF_Frame f;
    int16_t n;
    SF_Event ev = SF_EV_NONE;
    uint16_t i;

    memset(payload, 0x33, 10);
    SF_parserInit(&p);
    n = SF_build(SF_TYPE_CMD, 0, 0, SF_CID_DRIVE, payload, 10, seg, sizeof(seg));
    CHECK(n > 0);
    seg[SF_HEADER_LEN + 10u - 1u] ^= 0x01;     /* flip one payload bit */

    for (i = 0u; i < (uint16_t)n; i++)
    {
        ev = SF_parserFeed(&p, seg[i], i, &f);
    }
    CHECK_EQ(ev, SF_EV_CRC_ERR);
    CHECK_EQ(p.stats.frames, 0);
    CHECK_EQ(p.stats.crcErr, 1);
}

static void test_bad_version_len(void)
{
    static const uint8_t badVer[] = {0x5A, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t bigLen[] = {0x5A, 0x01, 0x01, 0x00, 0x00, 0xF9, 0x00, 0x00};
    SF_Parser p;

    SF_parserInit(&p);
    feed_all(&p, badVer, (uint16_t)sizeof(badVer), 0);
    CHECK_EQ(p.stats.verErr, 1);
    feed_all(&p, bigLen, (uint16_t)sizeof(bigLen), 0);
    CHECK_EQ(p.stats.fmtErr, 1);
    CHECK_EQ(p.stats.frames, 0);
}

/* ---------------- SEQ forward window ---------------- */
static void test_seq_window(void)
{
    SF_Parser p;
    SF_Frame f;
    int16_t n;
    uint16_t off;
    uint16_t accepted = 0;
    uint16_t rejected = 0;
    size_t k;
    static const uint8_t seqs[] = {10, 11, 43, 42, 44, 76, 77};

    memset(payload, 0x11, 2);

    off = 0;
    SF_parserInit(&p);
    for (k = 0u; k < sizeof(seqs); k++)
    {
        n = SF_build(SF_TYPE_CMD, seqs[k], 0, SF_CID_DRIVE, payload, 2, &seg[off],
                     (uint16_t)(sizeof(seg) - off));
        CHECK(n > 0);
        off = (uint16_t)(off + n);
    }
    for (uint16_t i = 0u; i < off; i++)
    {
        SF_Event ev = SF_parserFeed(&p, seg[i], i, &f);
        if (ev == SF_EV_FRAME)
        {
            accepted++;
        }
        else if (ev == SF_EV_SEQ_ERR)
        {
            rejected++;
        }
    }
    /* 10 first, 11 (+1), 43 (+32 edge) accepted; 42 (-1) rejected;
     * then 44 (+2), 76 (+32), 77 (+1) accepted -> 6 / 1 */
    CHECK_EQ(accepted, 6);
    CHECK_EQ(rejected, 1);
    CHECK_EQ(p.stats.seqErr, 1);
    CHECK_EQ(p.lastSeq, 77);

    /* wrap across 255 -> 0 is a normal advance */
    SF_parserInit(&p);
    n = SF_build(SF_TYPE_CMD, 255, 0, SF_CID_DRIVE, payload, 2, frame, sizeof(frame));
    feed_all(&p, frame, (uint16_t)n, 0);
    CHECK_EQ(p.stats.frames, 1);
    n = SF_build(SF_TYPE_CMD, 5, 0, SF_CID_DRIVE, payload, 2, frame, sizeof(frame));
    feed_all(&p, frame, (uint16_t)n, 1);
    CHECK_EQ(p.stats.frames, 2);
    n = SF_build(SF_TYPE_CMD, 4, 0, SF_CID_DRIVE, payload, 2, frame, sizeof(frame));
    feed_all(&p, frame, (uint16_t)n, 2);
    CHECK_EQ(p.stats.frames, 2);               /* behind the window */
    CHECK_EQ(p.stats.seqErr, 1);

    CHECK_EQ(SF_seqOk(1, 0), 1);
    CHECK_EQ(SF_seqOk(0, 0), 0);               /* duplicate */
    CHECK_EQ(SF_seqOk(32, 0), 1);
    CHECK_EQ(SF_seqOk(33, 0), 0);
}

/* ---------------- SEQ re-lock after a sender restart ----------------
 * A restarted sender's counter sits outside the window for good: without the
 * re-lock every frame is rejected until the u8 wraps (up to 224 frames). After
 * SF_SEQ_RELOCK_RUN rejects the window is dropped and the next frame is
 * accepted, whatever its seq. */
static void test_seq_relock(void)
{
    SF_Parser p;
    int16_t n;
    uint8_t k;
    uint16_t rejected = 0;

    memset(payload, 0x33, 2);

    SF_parserInit(&p);
    n = SF_build(SF_TYPE_CMD, 200, 0, SF_CID_DRIVE, payload, 2, frame,
                 sizeof(frame));
    CHECK(n > 0);
    feed_all(&p, frame, (uint16_t)n, 0);
    CHECK_EQ(p.stats.frames, 1);
    CHECK_EQ(p.lastSeq, 200);

    /* sender restarts at seq 0: 0,1,2,... are all outside the window
     * (200 -> delta 56). The first SF_SEQ_RELOCK_RUN frames must reject,
     * then the next one is accepted wherever the counter stands. */
    for (k = 0u; k < (uint8_t)(SF_SEQ_RELOCK_RUN + 1u); k++)
    {
        n = SF_build(SF_TYPE_CMD, k, 0, SF_CID_DRIVE, payload, 2, frame,
                     sizeof(frame));
        CHECK(n > 0);
        feed_all(&p, frame, (uint16_t)n, (uint32_t)(10u * k) + 100u);
        if (p.stats.frames == 2u)
        {
            break;             /* re-locked on this frame */
        }
        rejected++;
    }
    CHECK_EQ(rejected, SF_SEQ_RELOCK_RUN);
    CHECK_EQ(p.stats.frames, 2);
    CHECK_EQ(p.lastSeq, (uint8_t)SF_SEQ_RELOCK_RUN);
    CHECK_EQ(p.haveLastSeq, 1);

    /* and the stream continues normally from there */
    n = SF_build(SF_TYPE_CMD, (uint8_t)(SF_SEQ_RELOCK_RUN + 1u), 0,
                 SF_CID_DRIVE, payload, 2, frame, sizeof(frame));
    CHECK(n > 0);
    feed_all(&p, frame, (uint16_t)n, 300);
    CHECK_EQ(p.stats.frames, 3);
}

/* ---------------- residual frame timeout (cut FRAG segment) ---------------- */
static void test_residual_timeout(void)
{
    SF_Parser p;
    int16_t n;

    memset(payload, 0x22, 40);
    SF_parserInit(&p);
    n = SF_build(SF_TYPE_CMD, 0, SF_FLAG_FRAG, SF_CID_OTA_CHUNK, payload, 40,
                 seg, sizeof(seg));
    CHECK_EQ(n, 52);

    feed_all(&p, seg, 20, 1000);               /* transaction cut short      */
    CHECK_EQ(SF_parserBusy(&p), 1);
    CHECK_EQ(SF_parserTick(&p, 1003), SF_EV_NONE);     /* still inside 4 ms */
    CHECK_EQ(SF_parserTick(&p, 1004), SF_EV_TIMEOUT);  /* expired           */
    CHECK_EQ(p.stats.timeouts, 1);
    CHECK_EQ(SF_parserBusy(&p), 0);
    CHECK_EQ(SF_parserTick(&p, 9999), SF_EV_NONE);     /* idempotent        */

    /* leftovers from the cut segment are junk; a fresh frame still parses */
    feed_all(&p, &seg[20], (uint16_t)(n - 20), 1010);
    (void)SF_parserTick(&p, 2000);
    n = SF_build(SF_TYPE_CMD, 1, 0, SF_CID_DRIVE, payload, 4, frame, sizeof(frame));
    feed_all(&p, frame, (uint16_t)n, 2010);
    CHECK_EQ(p.stats.frames, 1);
    CHECK_EQ(p.lastSeq, 1);
}

/* ---------------- memory safety under a byte storm ---------------- */
#define GUARD_BYTES 64u
#define GUARD_BYTE  0xA5u

/* The parser is placed inside an over-allocated block whose head and tail are
 * filled with a guard pattern, so any write past buf[] is caught. */
static void storm(size_t bytes, uint8_t injectValid)
{
    uint8_t *block = malloc(GUARD_BYTES + sizeof(SF_Parser) + GUARD_BYTES);
    SF_Parser *p;
    SF_Frame f;
    size_t i;
    uint32_t rng = 0xBADF00Du;
    size_t injected = 0;
    size_t accepted = 0;
    int ok = 1;

    if (block == NULL)
    {
        printf("FAIL storm: out of memory\n");
        g_failed++;
        return;
    }
    memset(block, (int)GUARD_BYTE, GUARD_BYTES + sizeof(SF_Parser) + GUARD_BYTES);
    p = (SF_Parser *)(void *)(block + GUARD_BYTES);
    SF_parserInit(p);

    for (i = 0u; ok && (i < bytes); i++)
    {
        uint8_t b;

        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;

        if (injectValid && ((rng % 300u) < 2u))
        {
            /* splice a whole well-formed frame into the noise */
            uint16_t len = (uint16_t)(rng % (SF_MAX_PAYLOAD + 1u));
            int16_t n;
            uint16_t k;

            for (k = 0u; k < len; k++)
            {
                payload[k] = (uint8_t)(rng >> (k & 3u));
            }
            /* expire whatever the noise left half-parsed, then continue the
             * SEQ chain from the parser's own state */
            (void)SF_parserTick(p, (uint32_t)i + SF_RESIDUAL_TIMEOUT_MS + 1u);
            n = SF_build((uint8_t)(SF_TYPE_CMD + (rng % 6u)),
                         (uint8_t)(p->lastSeq + 1u), 0, SF_CID_DRIVE,
                         len ? payload : NULL, len, seg, sizeof(seg));
            if (n > 0)
            {
                for (k = 0u; k < (uint16_t)n; k++)
                {
                    if (SF_parserFeed(p, seg[k], (uint32_t)i, &f) == SF_EV_FRAME)
                    {
                        accepted++;
                    }
                }
                injected++;
            }
            continue;
        }

        b = (uint8_t)(rng >> ((i & 3u) * 8u));
        (void)SF_parserFeed(p, b, (uint32_t)i, &f);

        if ((p->idx > SF_MAX_FRAME) || (p->need > SF_MAX_PAYLOAD) || (p->state > 4u))
        {
            printf("FAIL storm: parser state out of range at byte %lu "
                   "(state=%u idx=%u need=%u)\n",
                   (unsigned long)i, p->state, p->idx, p->need);
            ok = 0;
        }
    }

    for (i = 0u; ok && (i < GUARD_BYTES); i++)
    {
        if ((block[i] != GUARD_BYTE) ||
            (block[GUARD_BYTES + sizeof(SF_Parser) + i] != GUARD_BYTE))
        {
            printf("FAIL storm: guard pattern clobbered at byte %lu\n",
                   (unsigned long)i);
            ok = 0;
        }
    }

    if (injectValid)
    {
        CHECK(injected > 0u);
        /* every injection is fed whole from a reset parser with the next SEQ */
        CHECK_EQ(accepted, injected);
    }
    if (ok)
    {
        printf("  storm(%s): %lu bytes, frames=%lu crc=%lu fmt=%lu ver=%lu "
               "seq=%lu to=%lu\n",
               injectValid ? "noise+frames" : "pure noise", (unsigned long)bytes,
               (unsigned long)p->stats.frames, (unsigned long)p->stats.crcErr,
               (unsigned long)p->stats.fmtErr, (unsigned long)p->stats.verErr,
               (unsigned long)p->stats.seqErr, (unsigned long)p->stats.timeouts);
        g_checks++;
    }
    free(block);
}

int main(void)
{
    uint16_t i;

    for (i = 0u; i < SF_MAX_PAYLOAD; i++)
    {
        payload[i] = (uint8_t)i;
    }

    test_crc_kat();
    test_wire_size();
    test_layout();
    test_build_errors();
    test_round_trip();
    test_multi_frame_segment();
    test_resync();
    test_crc_corruption();
    test_bad_version_len();
    test_seq_window();
    test_seq_relock();
    test_residual_timeout();
    storm(2000000u, 0u);
    storm(2000000u, 1u);

    printf("SF codec: %d checks, %d failures\n", g_checks, g_failed);
    return g_failed != 0 ? 1 : 0;
}
