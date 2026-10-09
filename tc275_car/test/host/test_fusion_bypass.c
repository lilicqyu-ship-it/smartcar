/* FUSION_CFG_PROTECTION=0 regression: the bench/diagnosis bypass must pass
 * the raw stick through untouched while every telemetry field stays live.
 * The production app/fusion.c is pulled in with the switch pre-defined to 0
 * (the #ifndef guard in fusion.h keeps it), so this binary exercises exactly
 * the code the firmware runs in that build - do not link app/fusion.c again.
 * Protected-mode behavior is covered by test_fusion.c; this file only asserts
 * what the bypass must and must not do. */
#define FUSION_CFG_PROTECTION 0
#include "app/fusion.c"
#include <assert.h>
#include <stdio.h>

static Fusion s;
static FusionInput in;

static void setup(void)
{
    unsigned i;
    FUSION_init(&s);
    memset(&in, 0, sizeof(in));
    in.fullScaleMmS = 1000;
    in.wheelsCalibrated = 1;
    in.imuAlive = 1;
    in.accMg[2] = 1000;
    in.tof.alive = 1;
    in.tof.zones = 16;
    for (i = 0; i < 16; i++)
    {
        in.tof.distanceMm[i] = 4000;
        in.tof.status[i] = 5;
        in.tof.targets[i] = 1;
    }
}

static void tick(int newFrame)
{
    in.nowMs += 10;
    in.imuSeq++;
    in.encoderSeq++;
    if (newFrame)
    {
        in.tof.seq++;
        in.tof.stampMs = in.nowMs;
    }
    FUSION_step(&s, &in);
}

int main(void)
{
    unsigned i;

    /* 1) A near obstacle must NOT stop or limit the bypass build - but the
     *    scene telemetry (nearest, cap, mode) must still report it. */
    setup();
    in.tof.distanceMm[7] = 40; /* one thin close return, tracked as valid */
    in.request[0] = in.request[1] = 1000;
    tick(1);
    assert(s.out.effective[0] == 1000 && s.out.effective[1] == 1000);
    assert(s.out.reason == FUSION_FREE && !s.out.brake);
    assert(s.out.nearestMm == 40);
    assert(s.out.capMmS == 0); /* advisory envelope: nearest below 60 mm */
    assert(s.tofMode == FUSION_MODE_TRACKED);
    assert(!(s.out.flags & FUSION_NEUTRAL_REQUIRED) && !s.latched);

    /* 2) The overspeed phantom latch (floor-bound cap < fs, wheels faster
     *    than cap + hysteresis, held far beyond the 100 ms window) must never
     *    fire in bypass: the car keeps obeying the stick. */
    in.wheelMmS[0] = in.wheelMmS[1] = 1100;
    in.tof.distanceMm[7] = 900; /* cap ~< fs, as ground returns bind it */
    for (i = 0; i < 30; i++)
    {
        tick(1);
        assert(s.out.effective[0] == 1000 && !s.out.brake && !s.latched);
        assert(s.out.reason == FUSION_FREE);
    }
    assert(s.out.speedMmS == 1100); /* telemetry anchor stayed live */

    /* 3) Release keeps the immediate-brake bit, but nothing latches and a
     *    fresh forward command drives immediately (no neutral requirement). */
    in.request[0] = in.request[1] = 0;
    tick(1);
    assert(s.out.effective[0] == 0 && s.out.brake == 1 && !s.latched);
    in.request[0] = in.request[1] = 1000;
    tick(1);
    assert(s.out.effective[0] == 1000 && s.out.reason == FUSION_FREE);

    /* 4) Encoder-lost and wheel-calibration guards are off: forward passes
     *    with default calibration and dead-side edges alike. */
    setup();
    in.wheelsCalibrated = 0;
    in.request[0] = in.request[1] = 800;
    tick(1);
    assert(s.out.effective[0] == 800 && s.out.reason == FUSION_FREE);
    setup();
    in.encEdgeAgeMs[0] = 0xFFFF; /* left side edge-dead */
    in.request[0] = in.request[1] = 800;
    for (i = 0; i < 60; i++) /* far beyond the 500 ms grace window */
        tick(1);
    assert(s.out.effective[0] == 800 && s.out.reason == FUSION_FREE && !s.latched);

    /* 5) BLIND (ToF dead/frozen) must not stop the bypass build. */
    setup();
    in.tof.alive = 0;
    in.request[0] = in.request[1] = 1000;
    for (i = 0; i < 30; i++)
        tick(1);
    assert(s.out.effective[0] == 1000 && s.out.reason == FUSION_FREE && !s.latched);
    assert(s.tofMode == FUSION_MODE_BLIND);

    /* 6) The 250 mm/s reverse cap is part of the protection envelope: off. */
    setup();
    in.request[0] = in.request[1] = -1000;
    tick(1);
    assert(s.out.effective[0] == -1000 && s.out.effective[1] == -1000);

    printf("test_fusion_bypass: PASS\n");
    return 0;
}
