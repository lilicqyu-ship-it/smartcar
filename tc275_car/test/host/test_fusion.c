#include "app/fusion.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Fusion s;
static FusionInput in;
static void setup(int distance)
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
        in.tof.distanceMm[i] = (int16_t)distance;
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
    int cap;
    uint8_t wire[FUSION_WIRE_LEN];
    int8_t axes[3] = {1, 2, 3};
    setup(4000);
    in.request[0] = in.request[1] = 1000;
    tick(1);
    assert(s.out.effective[0] == 1000 && !s.out.brake);
    /* One thin close obstacle must not be erased by spatial median filtering. */
    in.tof.distanceMm[7] = 100;
    tick(1);
    assert(s.out.brake && s.out.reason == FUSION_OBSTACLE && !s.out.effective[0]);
    in.tof.distanceMm[7] = 4000;
    for (i = 0; i < 20; i++)
        tick(1);
    assert(s.out.brake && (s.out.flags & FUSION_NEUTRAL_REQUIRED));
    in.request[0] = in.request[1] = 0;
    tick(1);
    assert(!s.latched);
    in.request[0] = in.request[1] = 1000;
    tick(1);
    assert(s.out.effective[0] == 1000);
    in.request[0] = in.request[1] = 0;
    tick(1);
    assert(s.out.brake);
    tick(1);
    assert(!s.out.brake); /* stationary motor jog/calibration still available */
    setup(100);
    in.request[0] = in.request[1] = -1000;
    tick(1);
    assert(s.out.effective[0] == -250 && !s.out.brake);
    setup(500);
    in.request[0] = 800;
    in.request[1] = 400;
    tick(1);
    assert(s.out.reason == FUSION_SLOW && s.out.effective[0] > 0 && s.out.effective[0] < 800);
    assert(abs(s.out.effective[0] - 2 * s.out.effective[1]) <= 1);
    cap = s.out.capMmS;
    in.wheelMmS[0] = (int16_t)(cap + 40);
    tick(1);
    assert(s.out.brake);
    setup(4000);
    in.request[0] = in.request[1] = 500;
    tick(1);
    for (i = 0; i < 26; i++)
        tick(0);
    assert(s.out.reason == FUSION_BLIND && s.out.brake);
    /* Repeated reads of one frame must not rearm the latch. */
    in.tof.stampMs = in.nowMs;
    in.request[0] = in.request[1] = 0;
    for (i = 0; i < 10; i++)
        tick(0);
    assert(s.latched);
    for (i = 0; i < 3; i++)
        tick(1);
    assert(!s.latched);
    setup(4000);
    in.request[0] = 500;
    for (i = 0; i < 16; i++)
        in.tof.status[i] = 0;
    tick(1);
    assert(s.out.reason == FUSION_SLOW && !s.out.brake);
    assert((s.out.flags & FUSION_TOF_LIMITED) && s.out.effective[0] == 150);
    setup(4000);
    in.request[0] = 500;
    for (i = 0; i < 16; i++)
        in.tof.targets[i] = 0;
    tick(1);
    assert(!(s.out.flags & FUSION_TOF_OK));
    assert(s.out.capMmS == 150 && s.out.effective[0] == 150 && !s.out.brake);
    /* One trusted close zone still stops, even with 15 unknown zones. */
    in.tof.targets[7] = 1;
    in.tof.distanceMm[7] = 100;
    tick(1);
    assert(s.out.reason == FUSION_OBSTACLE && s.out.brake);
    in.tof.distanceMm[7] = 1280;
    for (i = 0; i < 5; i++) tick(1);
    assert(s.latched); /* obstacle clearance cannot restart held throttle */
    in.request[0] = 0;
    tick(1);
    assert(!s.latched);
    in.request[0] = 500;
    tick(1);
    assert(s.out.effective[0] == 150 && !s.out.brake);
    for (i = 0; i < 26; i++) tick(0);
    assert(s.out.reason == FUSION_BLIND && s.out.brake); /* frozen sparse frame */
    setup(1280);
    for (i = 0; i < 16; i++) in.tof.targets[i] = 0;
    in.request[0] = in.request[1] = 500;
    in.wheelMmS[0] = 210;
    in.wheelMmS[1] = 150;
    for (i = 0; i < 10; i++) {
        tick(1);
        assert(!s.out.brake && s.out.effective[0] == 150);
    }
    in.wheelMmS[0] = 150;
    tick(1);
    assert(!s.overspeedSeen);
    in.wheelMmS[0] = 210;
    for (i = 0; i < 11; i++) tick(1);
    assert(s.out.brake && s.latched); /* sustained overspeed still stops */
    setup(1280);
    for (i = 0; i < 16; i++) in.tof.targets[i] = 0;
    in.tof.targets[0] = 1;
    in.tof.distanceMm[0] = 100;
    in.request[0] = 500;
    tick(1);
    assert(s.out.brake); /* sparse near obstacle does not wait 100 ms */
    setup(1280);
    for (i = 0; i < 16; i++) in.tof.targets[i] = 0;
    in.request[0] = 500;
    in.tof.alive = 0;
    tick(1);
    assert(s.out.reason == FUSION_BLIND && !(s.out.flags & FUSION_TOF_LIMITED));
    setup(1280);
    for (i = 0; i < 16; i++) in.tof.targets[i] = 0;
    in.request[0] = 500;
    in.wheelMmS[0] = 210;
    in.nowMs = 0xffffffa0u;
    for (i = 0; i < 10; i++) { tick(1); assert(!s.out.brake); }
    tick(1);
    assert(s.out.brake); /* overspeed timer across clock wrap */
    setup(4000);
    in.tof.zones = 65;
    in.request[0] = 500;
    tick(1);
    assert(s.out.brake);
    setup(4000);
    in.request[0] = 500;
    in.encoderSeq = 0;
    in.nowMs = 1;
    FUSION_step(&s, &in);
    assert(s.out.reason == FUSION_ENCODER_LOST);
    setup(4000);
    in.request[0] = 500;
    in.wheelsCalibrated = 0;
    tick(1);
    assert(s.out.brake);
    setup(4000);
    tick(1);
    in.request[0] = 500;
    for (i = 0; i < 7; i++)
    {
        in.nowMs += 10;
        in.tof.seq++;
        in.tof.stampMs = in.nowMs;
        FUSION_step(&s, &in);
    }
    assert(s.out.reason == FUSION_ENCODER_LOST);
    /* Delay and distance must monotonically reduce the velocity envelope. */
    setup(800);
    tick(1);
    cap = s.out.capMmS;
    tick(0);
    assert(s.out.capMmS < cap);
    in.tof.status[0] = 9;
    in.tof.distanceMm[0] = 400;
    tick(1);
    assert(s.out.nearestMm == 350);
    setup(4000);
    in.gyroMdps[2] = 2000;
    for (i = 0; i < 500; i++)
        tick(1);
    assert(s.out.flags & FUSION_BIAS_READY);
    assert(fabsf(s.bias[2] - 2.0f) < 0.001f);
    assert(!(s.out.flags & FUSION_CALIBRATED));
    assert(FUSION_calibrate(&s, axes, 160));
    s.cfg.straightAssist = 1;
    in.request[0] = in.request[1] = 300;
    in.gyroMdps[2] = 12000;
    for (i = 0; i < 20; i++)
        tick(1);
    assert(s.out.effective[0] != s.out.effective[1]);
    assert(s.out.headingCdeg > 0);
    in.wheelMmS[0] = -100;
    in.wheelMmS[1] = 100;
    tick(1);
    assert(s.out.flags & FUSION_SLIP);
    axes[2] = -3;
    assert(!FUSION_calibrate(&s, axes, 160));
    axes[2] = 2;
    assert(!FUSION_calibrate(&s, axes, 160));
    axes[2] = 3;
    assert(!FUSION_calibrate(&s, axes, 0));
    setup(350);
    assert(FUSION_calibrate(&s, (int8_t[]){1, 2, 3}, 160));
    s.biasSamples = 100;
    s.cfg.straightAssist = 1;
    s.holding = 1;
    s.heading = -100;
    s.holdHeading = 0;
    in.request[0] = in.request[1] = 300;
    tick(1);
    assert(s.out.effective[0] > 0 && s.out.effective[1] > 0);
    assert(s.out.effective[0] <= s.out.capMmS && s.out.effective[1] <= s.out.capMmS);
    setup(4000);
    assert(FUSION_calibrate(&s, (int8_t[]){1, 2, 3}, 160));
    s.biasSamples = 100;
    s.roll = 60;
    in.request[0] = in.request[1] = 200;
    tick(1);
    assert(s.out.reason == FUSION_TILT && s.out.brake);
    setup(4000);
    in.nowMs = 0xfffffff0u;
    tick(1);
    tick(0);
    tick(1);
    assert(s.out.flags & FUSION_TOF_OK);
    s.out.speedMmS = -123;
    FUSION_encode(&s.out, wire);
    assert(wire[0] == 1 && wire[8] == 0x85 && wire[9] == 0xff && wire[27] == s.out.brake);
    /* Replay all valid grid sizes, distances and commands: bounded outputs. */
    for (i = 1; i < 4000; i += 37)
    {
        setup((int)i);
        in.tof.zones = (i % 2) ? 16 : 64;
        if (in.tof.zones == 64)
        {
            unsigned j;
            for (j = 16; j < 64; j++)
            {
                in.tof.distanceMm[j] = (int16_t)i;
                in.tof.status[j] = 5;
                in.tof.targets[j] = 1;
            }
        }
        in.request[0] = 1000;
        in.request[1] = -500;
        tick(1);
        assert(s.out.effective[0] >= 0 && s.out.effective[0] <= 1000);
        assert(s.out.effective[1] >= -1000 && s.out.effective[1] <= 1000);
        assert(s.out.effective[0] * in.fullScaleMmS / 1000 <= s.out.capMmS);
    }
    puts("fusion scenarios passed (guard, freshness, calibration, bias, heading, slip, bounds)");
    return 0;
}
