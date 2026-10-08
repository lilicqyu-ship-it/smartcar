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
void FUSION_clearCalibration(Fusion *s)
{
    s->cfg.axis[0] = s->cfg.axis[1] = s->cfg.axis[2] = 0;
    s->cfg.trackMm = 0;
    s->heading = s->roll = s->pitch = s->yawRate = 0;
    s->holding = 0;
}
static void motion(Fusion *s, const FusionInput *in, float dt, uint32_t elapsedMs)
{
    unsigned i;
    int still = 1, freshImu, freshEnc, encOk, encBoth, liveSides;
    float acc[3], g[3], norm = 0, gyroMax = 0, meas;
    if (in->encoderSeq && (!s->encSeen || in->encoderSeq != s->encSeq))
    {
        s->encSeq = in->encoderSeq;
        s->encMs = in->nowMs;
        s->encSeen = 1;
    }
    freshEnc = s->encSeen && (uint32_t)(in->nowMs - s->encMs) <= 50u;
    /* Seq freshness only proves the CPU1 publisher task is alive - xcore
     * bumps it on every 1 ms publish even with all four Hall sensors dead.
     * Sensor health is judged per side from the edge age: a side must keep
     * producing edges while it is asked to drive, with a grace window for
     * the spin-up from standstill (the old single merged alive flag never
     * reached fusion at all and could not see a dead side behind a moving
     * opposite side, doc 51). Releasing the stick resets the window, so an
     * idle robot never latches. A latched protective stop has zeroed the
     * command itself: wheels at rest are then the expected consequence, so
     * the hold neither accrues toward ENCODER_LOST nor forgives a side that
     * was already edge-dead at latch time - health evaluation simply pauses,
     * and the stop keeps its original reason instead of relabelling after
     * 500 ms. */
    for (i = 0; i < 2; i++)
    {
        if (in->request[i] == 0)
            s->encAbsentMs[i] = 0;
        else if (s->latched)
            continue;
        else if (freshEnc && in->encEdgeAgeMs[i] <= FUSION_ENC_EDGE_FRESH_MS)
            s->encAbsentMs[i] = 0;
        else
            s->encAbsentMs[i] += elapsedMs;
    }
    encOk = freshEnc && s->encAbsentMs[0] < FUSION_ENC_GRACE_MS &&
            s->encAbsentMs[1] < FUSION_ENC_GRACE_MS;
    if (encOk)
        s->out.flags |= FUSION_ENCODER_OK;
    /* Speed anchor from the sides with live edges only: a dead side reads
     * 0 mm/s and would otherwise halve the reported speed. */
    meas = 0;
    liveSides = 0;
    if (freshEnc)
    {
        if (in->encEdgeAgeMs[0] <= FUSION_ENC_EDGE_FRESH_MS)
        {
            meas += in->wheelMmS[0];
            liveSides++;
        }
        if (in->encEdgeAgeMs[1] <= FUSION_ENC_EDGE_FRESH_MS)
        {
            meas += in->wheelMmS[1];
            liveSides++;
        }
        if (liveSides > 0)
            meas /= (float)liveSides;
    }
    s->velocity = meas; /* encoder anchor; IMU acceleration never free-integrates */
    /* Both sides live is the precondition for consuming the differential
     * wheel speed: one dead side fakes a huge or zero wheel yaw. */
    encBoth = freshEnc && in->encEdgeAgeMs[0] <= FUSION_ENC_EDGE_FRESH_MS &&
              in->encEdgeAgeMs[1] <= FUSION_ENC_EDGE_FRESH_MS;
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
            if (encBoth && in->wheelsCalibrated && s->biasSamples >= 100)
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
    int overspeed;
    int fs = in->fullScaleMmS >= 100 && in->fullScaleMmS <= 5000 ? in->fullScaleMmS : 1000;
    float dt = (float)(elapsed > 50u ? 10u : elapsed) * 0.001f;
    memset(o, 0, sizeof(*o));
    o->stampMs = in->nowMs;
    if (!s->started)
        s->stillMs = in->nowMs;
    s->started = 1;
    s->lastMs = in->nowMs;
    motion(s, in, dt, elapsed);
    o->tofAgeMs = (uint16_t)(age > 65535u ? 65535u : age);
    if (in->tof.alive && in->tof.seq && age <= 250u && (n == 16u || n == 64u))
    {
        unsigned width = n == 16u ? 4u : 8u;
        unsigned noTarget = 0, unknown = 0;
        /* Distinct new frames only: a frame re-read under the same seq must not
         * advance the open-clear streak (it is one physical sample, not two). */
        unsigned isNewFrame = (in->tof.seq != s->tofSeq);
        for (i = 0; i < n; i++)
        {
            int d = in->tof.distanceMm[i];
            uint8_t st = in->tof.status[i];
            if (in->tof.targets[i] && (st == 5 || st == 9) && d > 0 && d <= 4000)
            {
                valid++;
                /* Status 9 has lower confidence: enlarge margin by 50 mm. */
                if (st == 9)
                    d = d > 50 ? d - 50 : 1;
                side = (i % width) < width / 4 ? 0 : ((i % width) >= width - width / 4 ? 2 : 1);
                if (!o->sectorMm[side] || d < o->sectorMm[side])
                    o->sectorMm[side] = (uint16_t)d;
                if (!o->nearestMm || d < o->nearestMm)
                    o->nearestMm = (uint16_t)d;
            }
            else if (!in->tof.targets[i] && st == 255u)
                noTarget++; /* vendor "no target in this zone": decisive empty */
            else
                unknown++; /* anomalous / unexplainable: never counted as clear */
        }
        s->tofNoTarget = (uint8_t)noTarget;
        s->tofUnknown = (uint8_t)unknown;
        /* A frame whose statuses are mostly unexplainable is unreliable no
         * matter what distance it claims to see: it may only crawl (DEGRADED). */
        if (unknown * 100u > n * FUSION_OPEN_UNKNOWN_MAX_PCT)
        {
            o->flags |= FUSION_TOF_LIMITED;
            s->tofMode = FUSION_MODE_DEGRADED;
            s->openClearFrames = 0;
        }
        else if (valid > 0u)
        {
            /* At least one trusted distance: the stopping-distance envelope
             * alone sets the cap - a near obstacle binds (and the forward guard
             * stops within the margin), a far view cruises. Coverage is no
             * longer a proxy for uncertainty (fix-plan v1.0.9), so the stray
             * far returns that once locked an open field to 150 release it. */
            if (valid >= n / 2u && o->sectorMm[1])
                o->flags |= FUSION_TOF_OK;
            else
                o->flags |= FUSION_TOF_LIMITED;
            s->tofMode = FUSION_MODE_TRACKED;
            s->openClearFrames = 0;
        }
        else if (noTarget * 100u >= n * FUSION_OPEN_CLEAR_RATIO_PCT)
        {
            /* Healthy sensor facing decisive empty space - no trusted target
             * anywhere, most zones at status 255 "no target". Wire flag stays
             * LIMITED so every "usable frame" consumer is unchanged; the cap,
             * not the flag, lifts once several new frames agree. */
            o->flags |= FUSION_TOF_LIMITED;
            if (isNewFrame && s->openClearFrames < FUSION_OPEN_CLEAR_FRAMES)
                s->openClearFrames++;
            s->tofMode = s->openClearFrames >= FUSION_OPEN_CLEAR_FRAMES ? FUSION_MODE_OPEN
                                                                        : FUSION_MODE_DEGRADED;
        }
        else
        {
            o->flags |= FUSION_TOF_LIMITED;
            s->tofMode = FUSION_MODE_DEGRADED;
            s->openClearFrames = 0; /* neither empty nor trusted: re-arm */
        }
    }
    else
    {
        s->tofMode = FUSION_MODE_BLIND;
        s->tofNoTarget = 0;
        s->tofUnknown = 0;
        s->openClearFrames = 0;
    }
    o->validZones = (uint8_t)valid;
    if (s->tofMode == FUSION_MODE_OPEN)
    {
        /* Empty, healthy field: permit the open-space cruise cap instead of
         * collapsing a working sensor facing nothing to the crawl. */
        cap = fs < (int)FUSION_OPENSPACE_MM_S ? fs : (int)FUSION_OPENSPACE_MM_S;
    }
    else if (o->flags & (FUSION_TOF_OK | FUSION_TOF_LIMITED))
    {
        float clearance =
            o->nearestMm > s->cfg.marginMm ? (float)(o->nearestMm - s->cfg.marginMm) : 0;
        float a = (float)s->cfg.decelMmS2;
        float t = 0.18f + (float)age * 0.001f;
        cap = (int)(sqrtf(a * a * t * t + 2 * a * clearance) - a * t);
        if (cap > fs)
            cap = fs;
        /* Only a DEGRADED frame eases toward the crawl. TRACKED trusts its
         * measured distance envelope outright, so a far view or far obstacle is
         * capped by braking physics, not collapsed to 150 by a coverage
         * deficit. A persistently sparse/uncertain view must still decay rather
         * than pinch: a transient dip that dropped a few far returns had the
         * cap collapse ~900 to 150 in one step, put a moving robot above the
         * envelope, and the overspeed guard latched a full stop on every push
         * (bench 2026-10). DEGRADED decays from the last healthy cap at the
         * configured decel - a transient dip only eases the envelope, a
         * persistently unreliable view reaches the crawl within ~1 s of real
         * braking, and a genuinely closer target keeps binding below. */
        if (s->tofMode == FUSION_MODE_DEGRADED)
        {
            uint32_t since = (uint32_t)(in->nowMs - s->capContMs);
            int contCap;
            if (since > 20000u)
                since = 20000u;
            contCap = (int)s->capContMmS -
                      (int)(since * (uint32_t)s->cfg.decelMmS2 / 1000u);
            if (contCap < (int)FUSION_SPARSE_MM_S)
                contCap = (int)FUSION_SPARSE_MM_S;
            if (!o->nearestMm || cap > contCap)
                cap = contCap;
        }
    }
    o->capMmS = (uint16_t)cap;
    /* A confident (OK), distance-tracked (TRACKED) or confirmed-open (OPEN)
     * frame re-anchors the continuity envelope the DEGRADED path decays from:
     * each grants the speed the guard vouches to stop from within the margin,
     * so it is the right floor to hand time to. */
    if (s->tofMode == FUSION_MODE_OPEN || s->tofMode == FUSION_MODE_TRACKED ||
        (o->flags & FUSION_TOF_OK))
    {
        s->capContMmS = (uint16_t)cap;
        s->capContMs = in->nowMs;
    }
#if !FUSION_CFG_PROTECTION
    /* Telemetry-only bypass (FUSION_CFG_PROTECTION=0, see fusion.h): the scene
     * classification, cap and health flags above are the live truth, but none
     * of it may touch the motors - pass the raw stick through untouched. The
     * advisory capMmS stays on the wire so the [FUSION] log still shows what
     * the protection would have granted; with this build it enforces nothing. */
    o->effective[0] = l;
    o->effective[1] = r;
    o->brake = (uint8_t)(!l && !r && wasDriving);
    o->reason = FUSION_FREE;
    /* Keep the new-frame tracker and the latch state aligned with the bypass:
     * re-enabling protection must start from a clean slate, and the scene
     * classification above must keep seeing distinct frames. */
    s->tofSeq = in->tof.seq;
    s->latched = 0;
    s->overspeedSeen = 0;
    return;
#endif
    forward = l > 0 || r > 0;
    /* Overspeed is only a hazard while the envelope is actually TIGHTER than
     * the operator's ceiling. When the cap saturates at full scale (an open or
     * far view - fix-plan v1.0.9 releases these to fs), the fusion grants the
     * maximum it is willing to stop from and commands full; the motor's real
     * free-run speed then measures a little ABOVE the conservative
     * fullScaleMmS mapping (bench 2026-10: full throttle in empty space hit
     * ~1100 mm/s against a 1000 cap + 100 hysteresis, arming this guard and
     * latching a phantom stop_obstacle at nearest>1200 mm). That excursion is
     * a calibration artifact of "asked for full and got slightly more", not a
     * new obstacle - so it must not latch. Any cap below fs means a closer
     * view or a shrinking margin IS binding, and the guard stays armed there.
     */
    overspeed = cap < fs && (in->wheelMmS[0] > cap + (int)FUSION_OVERSPEED_HYST_MM_S ||
                              in->wheelMmS[1] > cap + (int)FUSION_OVERSPEED_HYST_MM_S);
    /* Encoder quantisation and PI startup transients are not a new obstacle:
     * the per-side window mean ripples tens of mm/s and a sparse-frame flap
     * can pinch the envelope far below the current speed with an unchanged
     * scene, so the excursion must clear the hysteresis and hold the window
     * before it counts. A real shrinking safety envelope violates it for far
     * longer - CPU1 ordinary slew cannot enforce one. */
    if (!forward || !overspeed)
        s->overspeedSeen = 0;
    else if (!s->overspeedSeen)
    {
        s->overspeedSeen = 1;
        s->overspeedMs = in->nowMs;
    }
    /* New distinct, healthy frames only count toward rearm hysteresis. */
    if (in->tof.seq != s->tofSeq)
    {
        s->tofSeq = in->tof.seq;
        if ((o->flags & (FUSION_TOF_OK | FUSION_TOF_LIMITED)) &&
            ((!o->nearestMm && (o->flags & FUSION_TOF_LIMITED)) ||
             o->nearestMm >= s->cfg.marginMm + 100u))
        {
            if (s->clearFrames < 3)
                s->clearFrames++;
        }
        else
            s->clearFrames = 0;
    }
    if (!(o->flags & (FUSION_TOF_OK | FUSION_TOF_LIMITED)))
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
        else if (!(o->flags & (FUSION_TOF_OK | FUSION_TOF_LIMITED)))
        {
            o->reason = FUSION_BLIND;
            hard = 1;
        }
        else if (o->nearestMm && o->nearestMm <= s->cfg.marginMm)
        {
            o->reason = FUSION_OBSTACLE;
            hard = 1;
        }
        else if (overspeed &&
                 (uint32_t)(in->nowMs - s->overspeedMs) >= FUSION_OVERSPEED_HOLD_MS)
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
