#include "app/fusion.h"
#include <math.h>
#include <string.h>
#define DEG 57.2957795f
static int absI(int x) { return x < 0 ? -x : x; }
static float wrap(float x)
{
    while (x > 180.0f)
        x -= 360.0f;
    while (x < -180.0f)
        x += 360.0f;
    return x;
}
static int16_t clamp(int x) { return (int16_t)(x > 1000 ? 1000 : (x < -1000 ? -1000 : x)); }
static float mapped(const int8_t *axis, const float *v, unsigned n)
{
    int a = axis[n];
    return a > 0 ? v[a - 1] : -v[-a - 1];
}
void FUSION_init(Fusion *s)
{
    memset(s, 0, sizeof(*s));
    s->cfg.marginMm = 220;
    s->cfg.decelMmS2 = 800;
    s->cfg.reverseMmS = 250;
}
uint8_t FUSION_calibrate(Fusion *s, const int8_t a[3], uint16_t track)
{
    int i, j, inv = 0, sign = 1;
    if (!a || track < 80 || track > 600)
        return 0;
    for (i = 0; i < 3; i++)
    {
        if (!a[i] || absI(a[i]) > 3)
            return 0;
        if (a[i] < 0)
            sign = -sign;
        for (j = 0; j < i; j++)
        {
            if (absI(a[i]) == absI(a[j]))
                return 0;
            if (absI(a[j]) > absI(a[i]))
                inv++;
        }
    }
    if ((inv % 2 ? -sign : sign) != 1)
        return 0;
    memcpy(s->cfg.axis, a, 3);
    s->cfg.trackMm = track;
    s->heading = s->roll = s->pitch = 0;
    s->holding = 0;
    return 1;
}
static void motion(Fusion *s, const FusionInput *in, float dt)
{
    unsigned i;
    int still = 1, freshImu, freshEnc;
    float acc[3], g[3], norm = 0, gyroMax = 0, meas;
    if (in->encoderSeq && (!s->encSeen || in->encoderSeq != s->encSeq))
    {
        s->encSeq = in->encoderSeq;
        s->encMs = in->nowMs;
        s->encSeen = 1;
    }
    freshEnc = s->encSeen && (uint32_t)(in->nowMs - s->encMs) <= 50u;
    if (freshEnc)
        s->out.flags |= FUSION_ENCODER_OK;
    meas = freshEnc ? (in->wheelMmS[0] + in->wheelMmS[1]) * 0.5f : 0;
    s->velocity = meas; /* encoder anchor; IMU acceleration never free-integrates */
    for (i = 0; i < 4; i++)
    {
        if (!s->countsSeen || in->counts[i] != s->counts[i])
            still = 0;
        s->counts[i] = in->counts[i];
    }
    s->countsSeen = 1;
    for (i = 0; i < 3; i++)
    {
        acc[i] = (float)in->accMg[i];
        norm += acc[i] * acc[i];
        g[i] = (float)in->gyroMdps[i] / 1000.0f;
        if (fabsf(g[i]) > gyroMax)
            gyroMax = fabsf(g[i]);
    }
    norm = sqrtf(norm);
    if (in->request[0] || in->request[1] || !freshEnc || absI(in->wheelMmS[0]) > 10 ||
        absI(in->wheelMmS[1]) > 10 || norm < 900 || norm > 1100 || gyroMax > 5 || !in->imuAlive)
        still = 0;
    if (!still)
        s->stillMs = in->nowMs;
    if (in->imuSeq && (!s->imuSeen || in->imuSeq != s->imuSeq))
    {
        float idt;
        idt = s->imuSeen ? (float)(uint32_t)(in->nowMs - s->imuMs) * 0.001f : dt;
        s->imuSeq = in->imuSeq;
        s->imuMs = in->nowMs;
        s->imuSeen = 1;
        if (idt > 0.05f)
            idt = 0.01f;
        if (in->imuAlive && still && (uint32_t)(in->nowMs - s->stillMs) >= 2000u)
        {
            float k = s->biasSamples < 200 ? 1.0f / (s->biasSamples + 1.0f) : 0.002f;
            for (i = 0; i < 3; i++)
                s->bias[i] += k * (g[i] - s->bias[i]);
            if (s->biasSamples < 200)
                s->biasSamples++;
        }
        for (i = 0; i < 3; i++)
            g[i] -= s->bias[i];
        if (in->imuAlive && s->cfg.axis[0])
        {
            float x = mapped(s->cfg.axis, acc, 0), y = mapped(s->cfg.axis, acc, 1),
                  z = mapped(s->cfg.axis, acc, 2);
            float gx = mapped(s->cfg.axis, g, 0), gy = mapped(s->cfg.axis, g, 1),
                  gz = mapped(s->cfg.axis, g, 2);
            float wheelYaw = (in->wheelMmS[1] - in->wheelMmS[0]) * DEG / s->cfg.trackMm;
            float yaw = gz;
            s->roll = wrap(s->roll + gx * idt);
            s->pitch = wrap(s->pitch + gy * idt);
            /* Reject gravity correction during impacts / strong acceleration. */
            if (norm > 850 && norm < 1150)
            {
                float k = idt / (0.5f + idt);
                s->roll += k * wrap(atan2f(y, z) * DEG - s->roll);
                s->pitch += k * wrap(atan2f(-x, sqrtf(y * y + z * z)) * DEG - s->pitch);
            }
            s->slip = 0;
            if (freshEnc && in->wheelsCalibrated && s->biasSamples >= 100)
            {
                if (fabsf(wheelYaw - gz) > 30)
                    s->slip = 1;
                else
                    yaw = 0.85f * gz + 0.15f * wheelYaw;
            }
            s->heading = wrap(s->heading + yaw * idt);
            s->yawRate = yaw;
        }
    }
    freshImu = s->imuSeen && in->imuAlive && (uint32_t)(in->nowMs - s->imuMs) <= 50u;
    if (freshImu)
        s->out.flags |= FUSION_IMU_OK;
    if (freshImu && s->slip)
        s->out.flags |= FUSION_SLIP;
    s->out.yawRateCdegS =
        (int16_t)(s->yawRate > 327.67f ? 32767
                                       : (s->yawRate < -327.67f ? -32767 : s->yawRate * 100.0f));
    if (s->cfg.axis[0])
        s->out.flags |= FUSION_CALIBRATED;
    if (s->biasSamples >= 100)
        s->out.flags |= FUSION_BIAS_READY;
    s->out.speedMmS = (int16_t)s->velocity;
    s->out.headingCdeg = (int16_t)(s->heading * 100);
    s->out.rollCdeg = (int16_t)(s->roll * 100);
    s->out.pitchCdeg = (int16_t)(s->pitch * 100);
}
void FUSION_step(Fusion *s, const FusionInput *in)
{
    FusionOutput *o = &s->out;
    int wasDriving = o->effective[0] != 0 || o->effective[1] != 0;
    unsigned i, n = in->tof.zones, side, valid = 0;
    uint32_t elapsed = s->started ? (uint32_t)(in->nowMs - s->lastMs) : 10u;
    uint32_t age = (uint32_t)(in->nowMs - in->tof.stampMs);
    int l = clamp(in->request[0]), r = clamp(in->request[1]), peak, cap = 0, hard = 0, forward;
    int fs = in->fullScaleMmS >= 100 && in->fullScaleMmS <= 5000 ? in->fullScaleMmS : 1000;
    float dt = (float)(elapsed > 50u ? 10u : elapsed) * 0.001f;
    memset(o, 0, sizeof(*o));
    o->stampMs = in->nowMs;
    if (!s->started)
        s->stillMs = in->nowMs;
    s->started = 1;
    s->lastMs = in->nowMs;
    motion(s, in, dt);
    o->tofAgeMs = (uint16_t)(age > 65535u ? 65535u : age);
    if (in->tof.alive && in->tof.seq && age <= 250u && (n == 16u || n == 64u))
    {
        unsigned width = n == 16u ? 4u : 8u;
        for (i = 0; i < n; i++)
        {
            int d = in->tof.distanceMm[i];
            if (!in->tof.targets[i] || (in->tof.status[i] != 5 && in->tof.status[i] != 9) ||
                d <= 0 || d > 4000)
                continue;
            valid++;
            /* Status 9 has lower confidence: enlarge margin by 50 mm. */
            if (in->tof.status[i] == 9)
                d = d > 50 ? d - 50 : 1;
            side = (i % width) < width / 4 ? 0 : ((i % width) >= width - width / 4 ? 2 : 1);
            if (!o->sectorMm[side] || d < o->sectorMm[side])
                o->sectorMm[side] = (uint16_t)d;
            if (!o->nearestMm || d < o->nearestMm)
                o->nearestMm = (uint16_t)d;
        }
        /* Sparse / invalid returns are unknown space, never an infinite range. */
        if (valid >= n / 2u && o->sectorMm[1])
            o->flags |= FUSION_TOF_OK;
    }
    o->validZones = (uint8_t)valid;
    if (o->flags & FUSION_TOF_OK)
    {
        float clearance =
            o->nearestMm > s->cfg.marginMm ? (float)(o->nearestMm - s->cfg.marginMm) : 0;
        float a = (float)s->cfg.decelMmS2;
        float t = 0.18f + (float)age * 0.001f;
        cap = (int)(sqrtf(a * a * t * t + 2 * a * clearance) - a * t);
        if (cap > fs)
            cap = fs;
    }
    o->capMmS = (uint16_t)cap;
    forward = l > 0 || r > 0;
    /* New distinct, healthy frames only count toward rearm hysteresis. */
    if (in->tof.seq != s->tofSeq)
    {
        s->tofSeq = in->tof.seq;
        if ((o->flags & FUSION_TOF_OK) && o->nearestMm >= s->cfg.marginMm + 100u)
        {
            if (s->clearFrames < 3)
                s->clearFrames++;
        }
        else
            s->clearFrames = 0;
    }
    if (!(o->flags & FUSION_TOF_OK))
        s->clearFrames = 0;
    if (!l && !r && s->clearFrames >= 3)
        s->latched = 0;
    if (forward)
    {
        if (!(o->flags & FUSION_ENCODER_OK) || !in->wheelsCalibrated)
        {
            o->reason = FUSION_ENCODER_LOST;
            hard = 1;
        }
        else if (!(o->flags & FUSION_TOF_OK))
        {
            o->reason = FUSION_BLIND;
            hard = 1;
        }
        else if (o->nearestMm <= s->cfg.marginMm)
        {
            o->reason = FUSION_OBSTACLE;
            hard = 1;
        }
        else if (in->wheelMmS[0] > cap + 30 || in->wheelMmS[1] > cap + 30)
        {
            /* CPU1 ordinary slew cannot enforce a shrinking safety envelope. */
            o->reason = FUSION_OBSTACLE;
            hard = 1;
        }
        if (hard)
            s->latched = 1;
        if (s->latched)
        {
            if (!o->reason)
                o->reason = FUSION_OBSTACLE;
            l = r = 0;
            hard = 1;
        }
        else
        {
            peak = l > r ? l : r;
            if (peak * fs / 1000 > cap)
            {
                int limit = cap * 1000 / fs;
                l = l * limit / peak;
                r = r * limit / peak;
                o->reason = FUSION_SLOW;
            }
        }
    }
    else
    {
        peak = absI(l) > absI(r) ? absI(l) : absI(r);
        if (peak * fs / 1000 > s->cfg.reverseMmS)
        {
            int limit = s->cfg.reverseMmS * 1000 / fs;
            l = l * limit / peak;
            r = r * limit / peak;
        }
    }
    if ((o->flags & (FUSION_CALIBRATED | FUSION_IMU_OK | FUSION_BIAS_READY)) ==
            (FUSION_CALIBRATED | FUSION_IMU_OK | FUSION_BIAS_READY) &&
        (fabsf(s->roll) > 45 || fabsf(s->pitch) > 45))
    {
        l = r = 0;
        hard = 1;
        s->latched = 1;
        o->reason = FUSION_TILT;
    }
    /* Heading hold is opt-in after measured geometry + axes + stationary bias. */
    if (s->cfg.straightAssist && !hard && l > 50 && absI(l - r) < 20 &&
        (o->flags & (FUSION_IMU_OK | FUSION_CALIBRATED | FUSION_BIAS_READY | FUSION_ENCODER_OK)) ==
            (FUSION_IMU_OK | FUSION_CALIBRATED | FUSION_BIAS_READY | FUSION_ENCODER_OK) &&
        !(o->flags & FUSION_SLIP))
    {
        int correction;
        int trimLimit = (l < r ? l : r) / 4;
        if (trimLimit > 80)
            trimLimit = 80;
        if (!s->holding)
        {
            s->holdHeading = s->heading;
            s->holding = 1;
        }
        correction = (int)(wrap(s->holdHeading - s->heading) * 8.0f - s->out.yawRateCdegS * 0.01f);
        if (correction > trimLimit)
            correction = trimLimit;
        if (correction < -trimLimit)
            correction = -trimLimit;
        l = clamp(l - correction);
        r = clamp(r + correction);
        /* Heading trim cannot exceed the forward range envelope. */
        peak = l > r ? l : r;
        if (peak * fs / 1000 > cap)
        {
            int limit = cap * 1000 / fs;
            l = l * limit / peak;
            r = r * limit / peak;
        }
    }
    else
        s->holding = 0;
    if (s->latched)
        o->flags |= FUSION_NEUTRAL_REQUIRED;
    o->effective[0] = (int16_t)l;
    o->effective[1] = (int16_t)r;
    o->brake = (uint8_t)(hard || (!l && !r && wasDriving));
}
static void u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
void FUSION_encode(const FusionOutput *o, uint8_t p[FUSION_WIRE_LEN])
{
    unsigned i;
    memset(p, 0, FUSION_WIRE_LEN);
    p[0] = 1;
    p[1] = o->reason;
    u16(p + 2, o->flags);
    u16(p + 4, o->nearestMm);
    u16(p + 6, o->capMmS);
    u16(p + 8, (uint16_t)o->speedMmS);
    u16(p + 10, (uint16_t)o->yawRateCdegS);
    u16(p + 12, (uint16_t)o->headingCdeg);
    u16(p + 14, (uint16_t)o->rollCdeg);
    u16(p + 16, (uint16_t)o->pitchCdeg);
    u16(p + 18, o->tofAgeMs);
    for (i = 0; i < 3; i++)
        u16(p + 20 + i * 2, o->sectorMm[i]);
    p[26] = o->validZones;
    p[27] = o->brake;
}
