/*
 * test_proto.c - host selftest for the vendored proto v2 codec.
 *
 * Scope mirrors the esp32c6_car host suite's most load-bearing assertions; the
 * codec itself is byte-identical to esp32c6_car/components/c6_proto (which
 * carries the 10^7 fuzz and randomized-frame suites), so only the CRC check
 * value and a frame roundtrip are re-asserted here to catch accidental
 * divergence of the vendored copy.
 */
#include <stdio.h>
#include <string.h>

#include "proto_frames.h"

static int failures;

#define CHECK(cond, name)                                                    \
    do {                                                                     \
        if (cond) {                                                          \
            printf("  ok    %s\n", name);                                    \
        } else {                                                             \
            printf("  FAIL  %s\n", name);                                    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_crc_check_value(void)
{
    /* CRC16-CCITT-FALSE catalogue value (esp32c6_car G1 gate assertion) */
    uint16_t c = proto_crc16((const uint8_t *)"123456789", 9);
    CHECK(c == 0x29B1, "crc16 check value 0x29B1");
    CHECK(proto_crc16(NULL, 8) == 0, "crc16 NULL guard");
}

static void test_drive_roundtrip(void)
{
    /* DRIVE 0x50 {v=600 mm/s, w=300 deg/s} as the ctrl task builds it */
    uint8_t data[4];
    proto_put_u16(&data[0], 600);
    proto_put_u16(&data[2], 300);

    uint8_t frame[PROTO_MAX_FRAME];
    size_t n = proto_build(PROTO_CMD_DRIVE, 7, data, sizeof(data),
                           frame, sizeof(frame));
    CHECK(n == 6 + 4 + 2, "frame length 12");
    CHECK(frame[0] == 0xAA && frame[1] == 0x55 && frame[2] == 0x02, "sync+ver");
    CHECK(frame[3] == 0x50 && frame[4] == 7 && frame[5] == 4, "cmd/seq/len");

    proto_parser_t p;
    proto_parser_init(&p);
    proto_frame_t out;
    proto_rx_ev_t ev = PROTO_RX_NONE;
    for (size_t i = 0; i < n; i++) {
        ev = proto_parser_feed(&p, frame[i], &out);
    }
    CHECK(ev == PROTO_RX_FRAME, "parsed to FRAME event");
    CHECK(out.cmd == PROTO_CMD_DRIVE && out.seq == 7 && out.len == 4,
          "cmd/seq/len preserved");
    int16_t v = (int16_t)((uint16_t)out.data[0] | ((uint16_t)out.data[1] << 8));
    int16_t w = (int16_t)((uint16_t)out.data[2] | ((uint16_t)out.data[3] << 8));
    CHECK(v == 600 && w == 300, "LE payload v=600 w=300");
}

static void test_telemetry_roundtrip(void)
{
    proto_telemetry_t t = { 0 };
    t.seq = 0x11223344u;
    t.fault_code = 0x0042;
    t.v_meas_l = -800;
    t.v_meas_r = -750;
    t.battery_mv = 7400;
    t.battery_pct = 85;
    t.fw_ver = 0x00010203u;

    uint8_t buf[PROTO_TELEMETRY_LEN];
    CHECK(proto_telemetry_encode(&t, buf, sizeof(buf)) == PROTO_TELEMETRY_LEN,
          "telemetry encode 38 B");

    proto_telemetry_t r = { 0 };
    CHECK(proto_telemetry_decode(buf, sizeof(buf), &r) == 0, "decode ok");
    CHECK(r.seq == t.seq && r.fault_code == t.fault_code, "u32/u16 LE fields");
    CHECK(r.v_meas_l == -800 && r.v_meas_r == -750, "negative i16 fields");
    CHECK(r.battery_pct == 85 && r.fw_ver == 0x00010203u, "u8/u32 fields");
}

static void test_parser_resync(void)
{
    /* garbage then a valid frame: parser must resync on the fly (doc 02 §5) */
    uint8_t data[2] = { 0x01, 0x00 };
    uint8_t frame[PROTO_MAX_FRAME];
    size_t n = proto_build(PROTO_CMD_STOP, 1, data, sizeof(data),
                           frame, sizeof(frame));

    proto_parser_t p;
    proto_parser_init(&p);
    proto_frame_t out;
    proto_rx_ev_t ev = PROTO_RX_NONE;

    static const uint8_t junk[] = { 0x00, 0xAA, 0x55, 0x00, 0xFF, 0xAA };
    for (size_t i = 0; i < sizeof(junk); i++) {
        proto_parser_feed(&p, junk[i], &out);
    }
    for (size_t i = 0; i < n; i++) {
        ev = proto_parser_feed(&p, frame[i], &out);
    }
    CHECK(ev == PROTO_RX_FRAME && out.cmd == PROTO_CMD_STOP,
          "resync through junk to valid frame");
}

int main(void)
{
    printf("proto host selftest (vendored c6_proto)\n");
    test_crc_check_value();
    test_drive_roundtrip();
    test_telemetry_roundtrip();
    test_parser_resync();

    if (failures) {
        printf("FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("PASS: all cases\n");
    return 0;
}
