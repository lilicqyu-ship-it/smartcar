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
static void test_trusted_ratio(void)
{
    unsigned n, count, j;
    /* 穷举两种网格的覆盖边界；分母为全网格，不是已知区域。
     * 缺失区分别使用无目标与未知状态，不能被当作完全可信。 */
    for (n = 16; n <= 64; n *= 4)
        for (count = 0; count <= n; count++)
        {
            setup(100);
            in.tof.zones = (uint8_t)n;
            in.request[0] = 800;
            in.request[1] = 400;
            for (j = 0; j < n; j++)
            {
                in.tof.distanceMm[j] = 100;
                in.tof.status[j] = j < count ? 5 : (j % 2 ? 0 : 255);
                in.tof.targets[j] = j < count ? 1 : 0;
            }
            tick(1);
            if (count * 3 > n * 2)
            {
                assert(s.out.flags & FUSION_TOF_OK);
                assert(!(s.out.flags & FUSION_TOF_LIMITED));
                assert(s.out.capMmS < 800 && s.out.effective[0] < 800);
                assert(abs(s.out.effective[0] - 2 * s.out.effective[1]) <= 1);
            }
            else
            {
                assert(!(s.out.flags & FUSION_TOF_OK));
                assert(s.out.flags & FUSION_TOF_LIMITED);
                assert(s.out.capMmS == 1000 && s.out.effective[0] == 800);
                assert(s.out.effective[1] == 400 && !s.out.brake);
            }
        }
    /* 状态 9、无目标、越界距离均不能补足 10/16 的完全可信区。 */
    for (j = 0; j < 3; j++)
    {
        setup(100);
        in.request[0] = 800;
        for (count = 10; count < 16; count++)
        {
            if (j == 0) in.tof.status[count] = 9;
            if (j == 1) in.tof.targets[count] = 0;
            if (j == 2) in.tof.distanceMm[count] = 4001;
        }
        tick(1);
        assert(!(s.out.flags & FUSION_TOF_OK));
        assert(s.out.capMmS == 1000 && s.out.effective[0] == 800);
    }
}
static void test_distance_policy(void)
{
    unsigned n, j, distance, previous;
    for (n = 16; n <= 64; n *= 4)
    {
        previous = 0;
        for (distance = 59; distance <= 151; distance++)
        {
            setup((int)distance);
            in.tof.zones = (uint8_t)n;
            in.request[0] = in.request[1] = 1000;
            in.wheelMmS[0] = in.wheelMmS[1] = 1100;
            for (j = 0; j < n; j++)
            {
                in.tof.distanceMm[j] = (int16_t)distance;
                in.tof.status[j] = 5;
                in.tof.targets[j] = 1;
            }
            tick(1);
            if (distance < 60)
                assert(s.latched && s.out.brake && !s.out.effective[0]);
            else
            {
                assert(!s.latched && !s.out.brake && s.out.effective[0] > 0);
                assert(s.out.capMmS >= previous);
                previous = s.out.capMmS;
                if (distance < 150)
                    assert(s.out.capMmS < 1000 && s.out.reason == FUSION_SLOW);
                else
                    assert(s.out.capMmS == 1000 && s.out.effective[0] == 1000);
            }
        }
    }
    /* 状态 9 的保守 -50 mm 遥测修正不能把原始 100 mm 判成 <60 mm。
     * 即便其余区域完全可信，它也不参与限速距离；原始 59 mm 仍硬停。 */
    setup(4000);
    in.request[0] = 1000;
    in.tof.status[7] = 9;
    in.tof.distanceMm[7] = 100;
    tick(1);
    assert(s.out.nearestMm == 50 && s.out.effective[0] == 1000 && !s.out.brake);
    in.tof.distanceMm[7] = 59;
    tick(1);
    assert(s.out.brake && s.latched);
    /* 低覆盖也保留 <60 mm 硬停；80 mm 三帧 + 松杆解锁。 */
    setup(4000);
    for (j = 0; j < 16; j++) in.tof.targets[j] = 0;
    in.tof.targets[7] = 1;
    in.tof.distanceMm[7] = 59;
    in.request[0] = 1000;
    tick(1);
    assert(s.out.brake && s.latched);
    in.request[0] = 0;
    in.tof.distanceMm[7] = 79;
    for (j = 0; j < 4; j++) tick(1);
    assert(s.latched);
    in.tof.distanceMm[7] = 80;
    tick(1);
    tick(0);
    tick(0);
    assert(s.latched);
    tick(1);
    assert(s.latched);
    tick(1);
    assert(!s.latched);
    /* 满量程极值下 60 mm 的目标仍非零，避免换算截断成提前停车。 */
    setup(60);
    in.fullScaleMmS = 5000;
    in.request[0] = 1000;
    tick(1);
    assert(s.out.effective[0] > 0 && !s.out.brake);
}
int main(void)
{
    unsigned i;
    int cap;
    uint8_t wire[FUSION_WIRE_LEN];
    int8_t axes[3] = {1, 2, 3};
    test_trusted_ratio();
    test_distance_policy();
    setup(4000);
    in.request[0] = in.request[1] = 1000;
    tick(1);
    assert(s.out.effective[0] == 1000 && !s.out.brake);
    /* One thin close obstacle must not be erased by spatial median filtering. */
    in.tof.distanceMm[7] = 40;
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
    setup(100);
    in.request[0] = 800;
    in.request[1] = 400;
    tick(1);
    assert(s.out.reason == FUSION_SLOW && s.out.effective[0] > 0 && s.out.effective[0] < 800);
    assert(abs(s.out.effective[0] - 2 * s.out.effective[1]) <= 1);
    /* 距离 >=60 mm 时，即使实测轮速持续高于目标也只降速、不锁存停车。 */
    in.wheelMmS[0] = in.wheelMmS[1] = 1150;
    for (i = 0; i < 30; i++) tick(1);
    assert(!s.out.brake && !s.latched && s.out.effective[0] > 0);
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
    assert(s.out.reason == FUSION_FREE && !s.out.brake);
    assert((s.out.flags & FUSION_TOF_LIMITED) && s.out.effective[0] == 500);
    setup(4000);
    in.request[0] = 500;
    for (i = 0; i < 16; i++)
        in.tof.targets[i] = 0;
    tick(1);
    assert(!(s.out.flags & FUSION_TOF_OK));
    assert(s.out.capMmS == 1000 && s.out.effective[0] == 500 && !s.out.brake);
    /* One trusted close zone still stops, even with 15 unknown zones. */
    in.tof.targets[7] = 1;
    in.tof.distanceMm[7] = 40;
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
    assert(s.out.effective[0] == 500 && !s.out.brake);
    for (i = 0; i < 26; i++) tick(0);
    assert(s.out.reason == FUSION_BLIND && s.out.brake); /* frozen sparse frame */
    /* 覆盖不足无论持续多久都不爬行，也不以低置信包络锁存超速。 */
    setup(100);
    in.request[0] = in.request[1] = 1000;
    in.wheelMmS[0] = in.wheelMmS[1] = 800;
    tick(1);
    for (i = 0; i < 10; i++) in.tof.targets[i] = 0;
    for (i = 0; i < 130; i++) tick(1);
    assert((s.out.flags & FUSION_TOF_LIMITED) && !(s.out.flags & FUSION_TOF_OK));
    assert(s.out.capMmS == 1000 && s.out.effective[0] == 1000 && !s.latched);
    /* 恢复充分覆盖后立即按距离限速，不锁存提前停车。 */
    for (i = 0; i < 10; i++) in.tof.targets[i] = 1;
    tick(1);
    assert((s.out.flags & FUSION_TOF_OK) && s.out.capMmS < 800);
    for (i = 0; i < 12; i++) tick(1);
    assert(!s.out.brake && !s.latched);
    setup(1280);
    for (i = 0; i < 16; i++) in.tof.targets[i] = 0;
    in.tof.targets[0] = 1;
    in.tof.distanceMm[0] = 40;
    in.request[0] = 500;
    tick(1);
    assert(s.out.brake); /* sparse near obstacle does not wait 100 ms */
    setup(1280);
    for (i = 0; i < 16; i++) in.tof.targets[i] = 0;
    in.request[0] = 500;
    in.tof.alive = 0;
    tick(1);
    assert(s.out.reason == FUSION_BLIND && !(s.out.flags & FUSION_TOF_LIMITED));
    setup(500);
    in.request[0] = 500;
    in.wheelMmS[0] = 700;
    in.nowMs = 0xffffffa0u;
    for (i = 0; i < 10; i++) { tick(1); assert(!s.out.brake); }
    tick(1);
    assert(!s.out.brake && !s.latched); /* 回绕不影响只降速的策略 */
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
    in.tof.distanceMm[7] = 40;
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
    /* 旧帧仍执行距离限速，超过新鲜期则走盲区硬停。 */
    setup(100);
    tick(1);
    cap = s.out.capMmS;
    tick(0);
    assert(s.out.capMmS == cap);
    in.tof.status[0] = 9;
    in.tof.distanceMm[0] = 80;
    tick(1);
    assert(s.out.nearestMm == 30);
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
    setup(100);
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
    /* 空旷分类仍需三个新帧，但不再施加 150/600 mm/s 上限。 */
    setup(0);
    for (i = 0; i < 16; i++)
    {
        in.tof.status[i] = 255;
        in.tof.targets[i] = 0;
        in.tof.distanceMm[i] = 0;
    }
    in.request[0] = in.request[1] = 1000;
    tick(1); /* streak 1: 已忽略限速，分类仍在确认 */
    assert((s.out.flags & FUSION_TOF_LIMITED) && !(s.out.flags & FUSION_TOF_OK));
    assert(s.out.capMmS == 1000 && s.out.effective[0] == 1000 && !s.out.brake);
    tick(1); /* streak 2 */
    assert(s.out.capMmS == 1000);
    tick(1); /* streak 3 -> OPEN_CLEAR */
    assert((s.out.flags & FUSION_TOF_LIMITED) && !(s.out.flags & FUSION_TOF_OK));
    assert(s.out.capMmS == 1000 && s.out.effective[0] == 1000 && !s.out.brake);
    /* 未知状态不算空旷，也不计入完全可信区；同样忽略限速。 */
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
    assert(s.out.capMmS == 1000 && s.out.effective[0] == 1000 && !s.out.brake);
    assert(s.tofMode == FUSION_MODE_DEGRADED && s.openClearFrames == 0);
    /* One trusted close zone among the empties stops it that same frame. */
    in.tof.status[7] = 5;
    in.tof.targets[7] = 1;
    in.tof.distanceMm[7] = 40;
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
