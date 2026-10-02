/*
 * test_cam_frame.c - host self-test for contracts/camera/cam_frame.h
 * (header-only codec; the remote copy in main/proto must stay byte-identical).
 *
 * Covers: wire layout LE encode/parse roundtrip, whole-frame rejects
 * (MAGIC/VER/LEN/JPEG), and the cam_seq_note drop/resync classification.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cam_frame.h"

static int s_fail;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            s_fail++;                                                        \
        }                                                                    \
    } while (0)

/* minimal fake JPEG: SOI + filler + EOI */
static size_t make_jpeg(uint8_t *buf, size_t len)
{
    buf[0] = 0xFF;
    buf[1] = 0xD8;
    for (size_t i = 2; i + 2 < len; i++) {
        buf[i] = (uint8_t)i;
    }
    buf[len - 2] = 0xFF;
    buf[len - 1] = 0xD9;
    return len;
}

static void test_roundtrip(void)
{
    uint8_t frame[CAM_OFF_PAYLOAD + 160];
    uint8_t jpeg[160];
    cam_frame_hdr_t h;
    cam_frame_hdr_t p;

    make_jpeg(jpeg, sizeof(jpeg));
    size_t n = cam_frame_build(0x01020304u, 0xAABBCCDDu, 320, 240,
                               (uint32_t)sizeof(jpeg), CAM_FLAG_KEYFRAME,
                               frame, sizeof(frame));
    CHECK(n == CAM_HEADER_LEN);
    memcpy(frame + CAM_OFF_PAYLOAD, jpeg, sizeof(jpeg));

    /* explicit little-endian wire layout */
    CHECK(frame[0] == CAM_MAGIC1 && frame[1] == CAM_MAGIC2);
    CHECK(frame[2] == 0x01 && frame[3] == CAM_FLAG_KEYFRAME);
    CHECK(frame[4] == 0x04 && frame[7] == 0x01);            /* SEQ LE   */
    CHECK(frame[8] == 0xDD && frame[11] == 0xAA);           /* TS LE    */
    CHECK(frame[12] == 0x40 && frame[13] == 0x01);          /* W  320   */
    CHECK(frame[14] == 0xF0 && frame[15] == 0x00);          /* H  240   */
    CHECK(frame[16] == 160 && frame[17] == 0);              /* LEN LE   */

    CHECK(cam_frame_parse(frame, sizeof(frame), &p) == CAM_RX_OK);
    CHECK(p.seq == 0x01020304u);
    CHECK(p.timestamp_ms == 0xAABBCCDDu);
    CHECK(p.width == 320 && p.height == 240);
    CHECK(p.jpeg_len == sizeof(jpeg));
    CHECK(p.flags == CAM_FLAG_KEYFRAME);

    /* reserved flag bits must be masked to 0 on TX */
    h = p;
    h.flags = 0xFF;
    CHECK(cam_frame_encode(&h, frame, sizeof(frame)) == CAM_HEADER_LEN);
    CHECK(frame[CAM_OFF_FLAGS] == (CAM_FLAG_KEYFRAME | CAM_FLAG_EVENT));

    /* encode argument validation */
    uint8_t small[CAM_HEADER_LEN - 1];
    CHECK(cam_frame_encode(&h, frame, sizeof(small)) == 0);
    h.jpeg_len = 0;
    CHECK(cam_frame_encode(&h, frame, sizeof(frame)) == 0);
    h.jpeg_len = CAM_JPEG_LEN_MAX + 1;
    CHECK(cam_frame_encode(&h, frame, sizeof(frame)) == 0);
    h.jpeg_len = sizeof(jpeg);
    h.width = 0;
    CHECK(cam_frame_encode(&h, frame, sizeof(frame)) == 0);
}

static void test_rejects(void)
{
    uint8_t frame[CAM_OFF_PAYLOAD + 160];
    uint8_t jpeg[160];
    cam_frame_hdr_t p;

    make_jpeg(jpeg, sizeof(jpeg));
    cam_frame_build(1, 1, 320, 240, sizeof(jpeg), 0, frame, sizeof(frame));
    memcpy(frame + CAM_OFF_PAYLOAD, jpeg, sizeof(jpeg));
    size_t len = sizeof(frame);

    CHECK(cam_frame_parse(frame, CAM_HEADER_LEN - 1, &p) == CAM_RX_TOO_SHORT);
    CHECK(cam_frame_parse(NULL, len, &p) == CAM_RX_TOO_SHORT);

    frame[1] ^= 0xFF;
    CHECK(cam_frame_parse(frame, len, &p) == CAM_RX_MAGIC_ERR);
    frame[1] = CAM_MAGIC2;

    frame[CAM_OFF_VERSION] = 0x02;
    CHECK(cam_frame_parse(frame, len, &p) == CAM_RX_VER_ERR);
    frame[CAM_OFF_VERSION] = CAM_FRAME_VER;

    cam_put_u32(&frame[CAM_OFF_JPEG_LEN], 0);
    CHECK(cam_frame_parse(frame, len, &p) == CAM_RX_LEN_ERR);
    cam_put_u32(&frame[CAM_OFF_JPEG_LEN], CAM_JPEG_LEN_MAX + 1);
    CHECK(cam_frame_parse(frame, len, &p) == CAM_RX_LEN_ERR);
    cam_put_u32(&frame[CAM_OFF_JPEG_LEN], (uint32_t)sizeof(jpeg));
    /* buffer shorter than header + payload -> whole-frame drop */
    CHECK(cam_frame_parse(frame, len - 1, &p) == CAM_RX_LEN_ERR);

    frame[CAM_OFF_PAYLOAD + 1] = 0x00;                        /* kill SOI */
    CHECK(cam_frame_parse(frame, len, &p) == CAM_RX_JPEG_ERR);
    frame[CAM_OFF_PAYLOAD + 1] = 0xD8;
    frame[CAM_OFF_PAYLOAD + sizeof(jpeg) - 1] = 0x00;         /* kill EOI */
    CHECK(cam_frame_parse(frame, len, &p) == CAM_RX_JPEG_ERR);
}

static void test_seq(void)
{
    uint32_t dropped = 123;

    /* first frame of the session resyncs, drop accounting starts fresh */
    CHECK(cam_seq_note(0, 0, 5, &dropped) == CAM_SEQ_RESYNC);
    CHECK(dropped == 0);

    CHECK(cam_seq_note(100, 1, 100, &dropped) == CAM_SEQ_STALE);  /* dup   */
    CHECK(cam_seq_note(100, 1, 99, &dropped) == CAM_SEQ_STALE);   /* small backward */
    CHECK(cam_seq_note(100, 1, 101, &dropped) == CAM_SEQ_FRESH);
    CHECK(dropped == 0);
    CHECK(cam_seq_note(100, 1, 105, &dropped) == CAM_SEQ_FRESH);
    CHECK(dropped == 4);                                         /* gap-1 */
    uint32_t gap512 = 100 + CAM_SEQ_GAP_MAX;
    CHECK(cam_seq_note(100, 1, gap512, &dropped) == CAM_SEQ_FRESH);
    CHECK(dropped == CAM_SEQ_GAP_MAX - 1);
    CHECK(cam_seq_note(100, 1, 100 + CAM_SEQ_GAP_MAX + 1, &dropped) == CAM_SEQ_RESYNC);
    CHECK(cam_seq_note(60000, 1, 10, &dropped) == CAM_SEQ_RESYNC); /* reboot rewind */
    CHECK(cam_seq_note(10, 1, 60000, &dropped) == CAM_SEQ_RESYNC); /* huge forward  */
}

int main(void)
{
    test_roundtrip();
    test_rejects();
    test_seq();
    if (s_fail != 0) {
        printf("cam_frame: %d FAILED\n", s_fail);
        return EXIT_FAILURE;
    }
    printf("cam_frame: all checks passed\n");
    return EXIT_SUCCESS;
}
