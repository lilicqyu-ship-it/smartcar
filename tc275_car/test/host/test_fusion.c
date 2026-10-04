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
    /* A single-step excursion is quantisation ripple, not an obstacle: it
     * must clear the hysteresis AND hold the window before it latches. */
    in.wheelMmS[0] = (int16_t)(cap + 40);
    tick(1);
    assert(!s.out.brake && !s.latched);
    in.wheelMmS[0] = (int16_t)(cap + FUSION_OVERSPEED_HYST_MM_S - 1);
    for (i = 0; i < 15; i++)
        tick(1);
    assert(!s.out.brake && !s.latched); /* below the hysteresis, however long */
    in.wheelMmS[0] = (int16_t)(cap + FUSION_OVERSPEED_HYST_MM_S + 1);
    for (i = 0; i < 9; i++)
    {
        tick(1);
        assert(!s.out.brake); /* 0..80 ms: inside the hold window */
    }
    tick(1);
    assert(!s.out.brake); /* 90 ms */
    tick(1);
    assert(s.out.brake && s.latched); /* 100 ms: sustained excursion stops */
    /* But when the envelope is NOT tighter than full scale - a far/open view
     * that pegs the cap at fs - the guard must not fire: the motor's real
     * free-run speed measures above the conservative fullScaleMmS mapping, so
     * wheel > cap + hysteresis is calibration, not a new obstacle (bench
     * 2026-10: full throttle in empty space latched a phantom stop_obstacle at
     * nearest>1200 mm). cap==fs disables the overspeed latch. */
    setup(4000);
    in.request[0] = in.request[1] = 1000;
    tick(1);
    assert(s.out.capMmS == 1000 && !s.out.brake);
    in.wheelMmS[0] = in.wheelMmS[1] = 1150; /* > cap + hysteresis, cap==fs */
    for (i = 0; i < 15; i++)
        tick(1); /* well past the 100 ms hold window */
    assert(!s.latched && !s.out.brake && s.out.effective[0] > 0);
    /* The exemption is only the full-scale grant: tighten the view and the same
     * sustained excursion latches again. */
    for (i = 0; i < 16; i++)
        in.tof.distanceMm[i] = 500; /* cap drops below fs */
    tick(1);
    assert(s.out.capMmS < 1000);
    in.wheelMmS[0] = in.wheelMmS[1] = (int16_t)(s.out.capMmS + 200);
    for (i = 0; i < 12; i++)
        tick(1);
    assert(s.out.brake && s.latched);
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
    in.wheelMmS[0] = 300;
    in.wheelMmS[1] = 150;
    for (i = 0; i < 10; i++) {
        tick(1);
        assert(!s.out.brake && s.out.effective[0] == 150);
    }
    in.wheelMmS[0] = 150;
    tick(1);
    assert(!s.overspeedSeen);
    in.wheelMmS[0] = 300;
    for (i = 0; i < 11; i++) tick(1);
    assert(s.out.brake && s.latched); /* sustained overspeed still stops */
    /* A transient sparse frame must not slam the envelope shut on a moving
     * robot: the cap decays from the last healthy value at the configured
     * decel instead of pinching to the crawl in one step (bench 2026-10:
     * valid_zones dipped 10 -> 7 for ~150 ms at ~370 mm/s and every push
     * latched a full stop). */
    setup(4000);
    in.request[0] = in.request[1] = 1000;
    in.wheelMmS[0] = in.wheelMmS[1] = 500;
    tick(1);
    assert((s.out.flags & FUSION_TOF_OK) && s.out.capMmS == 1000 && !s.out.brake);
    for (i = 0; i < 10; i++) /* ten far zones drop their returns */
        in.tof.targets[i] = 0;
    for (i = 0; i < 30; i++)
        tick(1);
    assert((s.out.flags & FUSION_TOF_LIMITED) && !s.out.brake && !s.latched);
    assert(s.out.capMmS > 700); /* 300 ms of decay, not an 850 mm/s pinch */
    for (i = 0; i < 10; i++)
        in.tof.targets[i] = 1;
    tick(1);
    assert((s.out.flags & FUSION_TOF_OK) && s.out.capMmS == 1000);
    /* A sparse view that persists still collapses to the crawl - by braking
     * down the decay, not by a step. */
    for (i = 0; i < 10; i++)
        in.tof.targets[i] = 0;
    for (i = 0; i < 130; i++)
    {
        in.wheelMmS[0] = in.wheelMmS[1] = (i < 70) ? 500 : 150;
        tick(1);
        assert(!s.out.brake);
    }
    assert(s.out.capMmS == FUSION_SPARSE_MM_S && !s.latched);
    /* A robot that fails to slow while the sparse view persists still gets
     * latched - against the decayed cap, once the hold window elapses. */
    in.wheelMmS[0] = in.wheelMmS[1] = 500;
    for (i = 0; i < 12; i++)
        tick(1);
    assert(s.out.brake && s.latched);
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
    in.wheelMmS[0] = 300;
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
    /* Sensors dead while CPU1 keeps publishing (doc 51): the seq check alone
     * sees a fresh publisher - xcore bumps it every 1 ms - so per-side edge
     * health must latch ENCODER_LOST after the grace window. */
    setup(4000);
    in.request[0] = in.request[1] = 500;
    in.wheelMmS[0] = in.wheelMmS[1] = 300;
    tick(1);
    assert(s.out.flags & FUSION_ENCODER_OK);
    in.encEdgeAgeMs[0] = 65535;
    in.encEdgeAgeMs[1] = 65535;
    for (i = 0; i < 49; i++)
        tick(1); /* 490 ms edge-silent under throttle: still within grace */
    assert(s.out.flags & FUSION_ENCODER_OK);
    tick(1); /* 500 ms: the commanded-but-silent sides count as dead */
    assert(!(s.out.flags & FUSION_ENCODER_OK));
    assert(s.out.reason == FUSION_ENCODER_LOST && s.out.brake && s.latched);
    /* One side dies, the opposite keeps turning (the masked-failure case the
     * merged alive flag could never see). The speed anchor must come from
     * the live side only, not average in the dead side's fake 0. */
    setup(4000);
    in.request[0] = in.request[1] = 500;
    in.wheelMmS[0] = in.wheelMmS[1] = 300;
    tick(1);
    in.encEdgeAgeMs[0] = 65535; /* left pair dies */
    in.wheelMmS[0] = 0;         /* ...and reads 0 mm/s */
    tick(1);
    assert(s.out.speedMmS == 300);
    for (i = 0; i < 50; i++)
        tick(1);
    assert(s.out.reason == FUSION_ENCODER_LOST && s.out.brake && s.latched);
    /* Health is only demanded while a side is asked to drive: idle with
     * stale edges never latches, and pushing off from standstill starts the
     * grace window instead of an immediate lockout. */
    setup(4000);
    in.encEdgeAgeMs[0] = 65535;
    in.encEdgeAgeMs[1] = 65535;
    for (i = 0; i < 30; i++)
        tick(1);
    assert((s.out.flags & FUSION_ENCODER_OK) && !s.out.brake);
    in.request[0] = in.request[1] = 500;
    in.wheelMmS[0] = in.wheelMmS[1] = 0;
    for (i = 0; i < 40; i++)
        tick(1); /* wheels still spinning up at 400 ms: no lockout */
    assert(!s.out.brake && s.out.reason != FUSION_ENCODER_LOST);
    in.encEdgeAgeMs[0] = in.encEdgeAgeMs[1] = 0; /* edges arrive */
    in.wheelMmS[0] = in.wheelMmS[1] = 300;
    for (i = 0; i < 60; i++)
        tick(1);
    assert(!s.out.brake && (s.out.flags & FUSION_ENCODER_OK));
    /* A protective hold must not relabel itself ENCODER_LOST after 500 ms:
     * holding the throttle keeps request > 0 while the stopped wheels go
     * edge-silent - the expected consequence of the stop, not a sensor
     * fault. The reason stays the original one and the guard re-arms from
     * zero, so the next push still gets the full spin-up grace window. */
    setup(4000);
    in.request[0] = in.request[1] = 1000;
    tick(1);
    in.tof.distanceMm[7] = 100;
    tick(1);
    assert(s.out.reason == FUSION_OBSTACLE && s.out.brake && s.latched);
    in.tof.distanceMm[7] = 4000;
    in.wheelMmS[0] = in.wheelMmS[1] = 0;
    in.encEdgeAgeMs[0] = in.encEdgeAgeMs[1] = 65535;
    for (i = 0; i < 60; i++)
        tick(1);
    assert(s.out.reason == FUSION_OBSTACLE && (s.out.flags & FUSION_ENCODER_OK));
    in.request[0] = in.request[1] = 0;
    tick(1);
    assert(!s.latched);
    in.encEdgeAgeMs[0] = in.encEdgeAgeMs[1] = 0;
    in.request[0] = in.request[1] = 1000;
    tick(1);
    assert(!s.out.brake && s.out.effective[0] == 1000);
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
    /* OPEN_CLEAR (fix-plan v1.0.9): a healthy ToF facing empty space must
     * cruise, not crawl. A fresh frame with no trusted target anywhere, most
     * zones at range status 255 "no target" and few unknowns opens the cap to
     * FUSION_OPENSPACE_MM_S only after several consecutive new frames; the
     * wire flag stays LIMITED so every not-blind consumer is unchanged, and a
     * single trusted close zone still stops the car that same frame. */
    setup(0);
    for (i = 0; i < 16; i++)
    {
        in.tof.status[i] = 255;
        in.tof.targets[i] = 0;
        in.tof.distanceMm[i] = 0;
    }
    in.request[0] = in.request[1] = 1000;
    tick(1); /* streak 1: still the crawl envelope, no single frame reopens */
    assert((s.out.flags & FUSION_TOF_LIMITED) && !(s.out.flags & FUSION_TOF_OK));
    assert(s.out.capMmS == FUSION_SPARSE_MM_S && s.out.effective[0] == 150 && !s.out.brake);
    tick(1); /* streak 2 */
    assert(s.out.capMmS == FUSION_SPARSE_MM_S);
    tick(1); /* streak 3 -> OPEN_CLEAR */
    assert((s.out.flags & FUSION_TOF_LIMITED) && !(s.out.flags & FUSION_TOF_OK));
    assert(s.out.capMmS == FUSION_OPENSPACE_MM_S && s.out.effective[0] == 600 && !s.out.brake);
    /* Anomalous empties (targets 0 but status != 255) are unknown, never read
     * as clear: a fresh anchor holds the crawl and the open streak never
     * builds, however many such frames arrive in a row. */
    setup(0);
    for (i = 0; i < 16; i++)
    {
        in.tof.status[i] = 5;
        in.tof.targets[i] = 0;
        in.tof.distanceMm[i] = 0;
    }
    in.request[0] = in.request[1] = 1000;
    for (i = 0; i < 6; i++)
        tick(1);
    assert(s.out.capMmS == FUSION_SPARSE_MM_S && s.out.effective[0] == 150 && !s.out.brake);
    assert(s.tofMode == FUSION_MODE_DEGRADED && s.openClearFrames == 0);
    /* One trusted close zone among the empties stops it that same frame. */
    in.tof.status[7] = 5;
    in.tof.targets[7] = 1;
    in.tof.distanceMm[7] = 100;
    tick(1);
    assert(s.out.brake && s.out.reason == FUSION_OBSTACLE && s.out.effective[0] == 0);
    /* Replaying the same stale seq must not re-anchor OPEN after a blind. */
    in.tof.alive = 0;
    in.request[0] = in.request[1] = 1000;
    tick(1);
    assert(s.out.reason == FUSION_BLIND && s.out.brake);
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
