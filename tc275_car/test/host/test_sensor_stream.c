/*
 * test_sensor_stream.c - host unit tests for the diagnostic sensor streams
 *
 * app/sensor_stream.c builds EVT 0x2A/0x2B payloads for the phone-side
 * sensor visualisation; everything testable here is the wire math: the
 * mm/16 zone quantisation with 0xFF for untrusted zones, the whole-frame
 * valid/nearest recomputation repeated in every fragment header, the
 * little-endian IMU layout and the rate limiting (20 Hz IMU, one zone
 * frame per FusionTof seq change).
 *
 * Build & run, same pattern as test_xcore.c (stub include dir FIRST so
 * Ifx_Types.h / IfxCpu.h resolve to the host stand-ins):
 *   gcc -std=c99 -Wall -Wextra -Werror -O2 -I test/host/stub -I . \
 *       test/host/test_sensor_stream.c app/sensor_stream.c mw/xcore/xcore.c \
 *       -o test/host/out/test_sensor_stream && test/host/out/test_sensor_stream
 */
#include "app/sensor_stream.h"
#include "bsp/tof.h"
#include "mw/sf/sf_frame.h"
#include "mw/xcore/xcore.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;

static void check(int cond, const char *what)
{
    g_checks++;
    if (!cond)
    {
        g_failed++;
        printf("FAIL: %s\n", what);
    }
}

static uint16_t rd16(const uint8 *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* xcore.c's log pump needs a UART stand-in on the host; the streams under
 * test never log, so a sink-to-nowhere keeps the linker happy. */
static uint8 g_uartSink[4096];
static size_t g_uartLen;
uint32 UART_printTry(const char *data, uint32 len)
{
    if (g_uartLen + len <= sizeof(g_uartSink))
    {
        memcpy(g_uartSink + g_uartLen, data, len);
        g_uartLen += len;
    }
    return len;
}

static uint32_t rd32(const uint8 *p)
{
    return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16);
}

static boolean drain(XcoreEvtFrame *f)
{
    if (!XCORE_dataEvtPeek(f))
    {
        return FALSE;
    }
    XCORE_dataEvtPop();
    return TRUE;
}

static void publishTof(uint32 seq, uint16 distance, unsigned validZones)
{
    FusionTof tof;
    unsigned i;

    memset(&tof, 0, sizeof(tof));
    tof.seq = seq;
    tof.stampMs = seq * 66u;
    tof.alive = TRUE;
    tof.zones = FUSION_MAX_ZONES;
    for (i = 0; i < FUSION_MAX_ZONES; i++)
    {
        tof.distanceMm[i] = (int16_t)(distance + (int16_t)i);
        tof.status[i] = (i < validZones) ? 5u : 255u; /* 255 = no target */
    }
    XCORE_tofPublish(&tof);
}

static void test_first_tick_emits_imu_and_three_fragments(void)
{
    XcoreImu imu;
    XcoreEvtFrame f;
    unsigned zoneFrames = 0;
    unsigned imuFrames = 0;
    unsigned fragSeen[3] = {0, 0, 0};
    unsigned i;

    memset(&imu, 0, sizeof(imu));
    imu.seq = 42u;
    imu.stampMs = 1234u;
    imu.alive = TRUE;
    imu.accMilliG[0] = -512;
    imu.accMilliG[1] = 250;
    imu.accMilliG[2] = 1000;
    imu.gyroMilliDps[0] = -200000; /* -200 dps, needs the i32 layout */
    imu.gyroMilliDps[1] = 12345;
    imu.gyroMilliDps[2] = -1;
    imu.tempCentiC = 2573; /* 25.73 degC */
    XCORE_imuPublish(&imu);
    publishTof(7u, 1000u, 60u);

    /* XCORE_imuPublish owns the seq bump (xcore.h), so the expected wire seq
     * is whatever the snapshot now reads back as. */
    {
        XcoreImu published;
        XCORE_imuRead(&published);
        imu.seq = published.seq;
    }

    SENSORSTREAM_tick(0u, (uint8_t)FUSION_MODE_OPEN);

    while (drain(&f))
    {
        check(f.type == (uint8)SF_TYPE_EVT, "frame is an EVT");
        if (f.cid == SENSORSTREAM_EVT_CID_IMU)
        {
            imuFrames++;
            check(f.len == SENSORSTREAM_IMU_WIRE_LEN, "imu frame length");
            check(rd32(f.payload + 0) == imu.seq, "imu seq (bumped by the publisher)");
            check(rd32(f.payload + 4) == 1234u, "imu stampMs");
            check((int16_t)rd16(f.payload + 8) == 2573, "imu tempCentiC");
            check((int16_t)rd16(f.payload + 10) == -512, "accX mg");
            check((int16_t)rd16(f.payload + 12) == 250, "accY mg");
            check((int16_t)rd16(f.payload + 14) == 1000, "accZ mg");
            check((int32_t)rd32(f.payload + 16) == -200000, "gyroX mdps i32");
            check((int32_t)rd32(f.payload + 20) == 12345, "gyroY mdps");
            check((int32_t)rd32(f.payload + 24) == -1, "gyroZ mdps");
        }
        else if (f.cid == SENSORSTREAM_EVT_CID_TOF_ZONES)
        {
            unsigned first;
            zoneFrames++;
            check(f.len == SENSORSTREAM_TOF_WIRE_LEN, "zone fragment length");
            check(rd16(f.payload + 0) == 7u, "fragment seq");
            check(f.payload[2] <= 2u, "fragment index range");
            check(f.payload[3] == (uint8_t)FUSION_MODE_OPEN, "fragment mode");
            check(f.payload[4] == 60u, "whole-frame valid count");
            check(rd16(f.payload + 5) == 1000u, "nearest = min valid zone");
            fragSeen[f.payload[2]] = 1u;
            first = (unsigned)f.payload[2] * SENSORSTREAM_TOF_FRAG_ZONES;
            for (i = 0; i < SENSORSTREAM_TOF_FRAG_ZONES; i++)
            {
                unsigned zone = first + i;
                uint8 cell = f.payload[SENSORSTREAM_TOF_HDR_LEN + i];
                if (zone >= FUSION_MAX_ZONES)
                {
                    check(cell == 0u, "fragment padding past zone 63 is zero");
                }
                else if (zone < 60u)
                {
                    /* mm/16 quantisation: 1000+zone mm -> (1000+zone)/16 */
                    check(cell == (uint8)((1000u + zone) / 16u), "zone quantised mm/16");
                }
                else
                {
                    check(cell == SENSORSTREAM_ZONE_INVALID, "untrusted zone is 0xFF");
                }
            }
        }
        else
        {
            check(0, "unexpected CID on the data ring");
        }
    }
    check(imuFrames == 1u, "exactly one IMU frame on the first tick");
    check(zoneFrames == 3u, "three zone fragments");
    check(fragSeen[0] && fragSeen[1] && fragSeen[2], "all fragment indexes seen");
}

static void test_rate_limiting(void)
{
    XcoreEvtFrame f;
    unsigned frames = 0;

    /* Same TOF seq, IMU period not elapsed: nothing new may be queued. */
    SENSORSTREAM_tick(10u, (uint8_t)FUSION_MODE_TRACKED);
    while (drain(&f))
    {
        frames++;
    }
    check(frames == 0u, "no frames between rate ticks");

    /* IMU period reached (50 ms), TOF seq still unchanged: one IMU frame. */
    SENSORSTREAM_tick(50u, (uint8_t)FUSION_MODE_TRACKED);
    while (drain(&f))
    {
        frames++;
        check(f.cid == SENSORSTREAM_EVT_CID_IMU, "only the IMU stream re-fires");
    }
    check(frames == 1u, "IMU re-fires on its own 50 ms period");

    /* New TOF frame 10 ms after the last IMU send -> only the 3 fragments
     * (the IMU stream re-fires on its own 50 ms period, not the TOF's). */
    publishTof(8u, 320u, 1u);
    SENSORSTREAM_tick(60u, (uint8_t)FUSION_MODE_TRACKED);
    frames = 0;
    while (drain(&f))
    {
        frames++;
        if (f.cid == SENSORSTREAM_EVT_CID_TOF_ZONES)
        {
            check(rd16(f.payload + 0) == 8u, "zone fragment carries the new seq");
            check(f.payload[4] == 1u, "valid count follows the new frame");
            check(rd16(f.payload + 5) == 320u, "nearest tracks the new frame");
        }
        else
        {
            check(f.cid == SENSORSTREAM_EVT_CID_IMU, "unexpected CID");
        }
    }
    check(frames == 3u, "new TOF frame: only the 3 fragments");
}

static void test_quantisation_clamps(void)
{
    FusionTof tof;
    XcoreEvtFrame f;
    unsigned found = 0;

    memset(&tof, 0, sizeof(tof));
    tof.seq = 9u;
    tof.alive = TRUE;
    tof.zones = FUSION_MAX_ZONES;
    tof.distanceMm[0] = -50;   /* negative clamps to 0 */
    tof.status[0] = 5u;
    tof.distanceMm[1] = 5000;  /* 5000/16 = 312 -> clamps to 255 */
    tof.status[1] = 9u;        /* 9 is the second OK status */
    tof.distanceMm[2] = 4064;  /* 4064/16 = 254, fits */
    tof.status[2] = 5u;
    /* zones 3..63 left at status 0 (not OK) -> 0xFF */
    XCORE_tofPublish(&tof);
    SENSORSTREAM_tick(120u, (uint8_t)FUSION_MODE_BLIND);

    while (drain(&f))
    {
        if (f.cid == SENSORSTREAM_EVT_CID_TOF_ZONES)
        {
            found++;
            check(f.payload[3] == (uint8_t)FUSION_MODE_BLIND, "blind mode passes through");
            if (f.payload[2] == 0u)
            {
                check(f.payload[SENSORSTREAM_TOF_HDR_LEN + 0] == 0u, "negative mm clamps to 0");
                check(f.payload[SENSORSTREAM_TOF_HDR_LEN + 1] == 255u, "312 cells clamp to 255");
                check(f.payload[SENSORSTREAM_TOF_HDR_LEN + 2] == 254u, "4064 mm -> 254");
                check(f.payload[SENSORSTREAM_TOF_HDR_LEN + 3] == SENSORSTREAM_ZONE_INVALID,
                      "status-0 zone is invalid");
                check(f.payload[4] == 3u, "valid counts only status 5/9");
                check(rd16(f.payload + 5) == 0u, "nearest clamps to 0 for the -50 mm zone");
            }
        }
    }
    check(found == 3u, "clamp frame still fragments into 3");
}

int main(void)
{
    /* The data ring is process-global state; the tests below rely on being
     * drained fully by the previous case (every case drains to empty). */
    test_first_tick_emits_imu_and_three_fragments();
    test_rate_limiting();
    test_quantisation_clamps();

    printf("sensor_stream: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
