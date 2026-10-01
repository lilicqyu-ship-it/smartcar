#include "rt/encoder.h"

#include "IfxGtm.h"
#include "IfxGtm_Cmu.h"
#include "IfxGtm_PinMap.h"
#include "IfxGtm_Tim.h"
#include "IfxPort.h"
#include "IfxSrc.h"
#include "IfxCpu.h"
#include "bsp/stime.h"
#include "mw/xcore/xcore.h"

/* Wheel Hall encoder decode, CPU1 (doc/20-design/23-wiring.md section 8,
 * doc/20-design/21-software-design.md section 5.1).
 *
 * The original design wanted GTM TIM UDC hardware quadrature (UDCCTRL/CLS/
 * DUTC registers). Those registers exist in GTM gen 2 but NOT in the TC275's
 * gen 3 TIM (verified against IfxGtm_regdef.h: no UDC register at all, TIM
 * modes are TPWM/TPIM/TIEM/TIPM/TBCM/TGPS only), and GPT12's incremental
 * inputs and the ERU channels are not routed to any brought-out pin. So the
 * decode is done in software, per edge, exactly like the WHEELTEC STM32
 * reference (TIM encoder mode TI12, 4x counting):
 *
 *   - every TIM0 channel runs in input event mode (TIEM) on both edges and
 *     raises its NEWVAL interrupt on the shared vector table (all three CPUs
 *     share table 0, see Lcf_Tasking_Tricore_Tc.lsl __INTTAB_CPU*);
 *   - the ISR reads the post-edge level of the edge's own phase and of the
 *     partner phase and applies the standard 4x quadrature table;
 *   - the 1 kHz ENCODER_task() takes per-ms deltas, applies the 8 ms mean
 *     window and publishes side speeds through xcore.
 *
 * Worst case ISR load: 4 encoders * 52 edges per motor rev at the MG310's
 * 500 rpm no-load spec = ~1.7k IRQ/s (~3k at the 13 V top of the recommended
 * range), ~0.3 us each -> negligible on CPU1.
 */

#define ENC_TIM                IfxGtm_Tim_0
#define ENC_FILTER_TIME_S      2e-6f    /* deglitch, open drain + pull-up rise */
#define ENC_WINDOW_MS          8u       /* doc 21 section 5.1 sliding window   */
#define ENC_ALIVE_WINDOW_MS    500u     /* doc 21 section 5.1 dead-encoder window */
#define ENC_ISR_PRIO_BASE      16u      /* free above 13, shared inttab        */

/* One entry per TIM channel, wiring table 23-wiring.md section 8.2:
 * ch0=E1A P33.4, ch1=E1B P33.5, ch2=E2A P33.6, ch3=E2B P33.7,
 * ch4=E3A P33.0, ch5=E3B P33.1, ch6=E4A P33.2, ch7=E4B P33.3.
 * A phases land on even channels, B phases on the odd partners. */
static const IfxGtm_Tim_TinMap *const g_tinPins[8] = {
    &IfxGtm_TIM0_0_TIN26_P33_4_IN,
    &IfxGtm_TIM0_1_TIN27_P33_5_IN,
    &IfxGtm_TIM0_2_TIN28_P33_6_IN,
    &IfxGtm_TIM0_3_TIN29_P33_7_IN,
    &IfxGtm_TIM0_4_TIN22_P33_0_IN,
    &IfxGtm_TIM0_5_TIN23_P33_1_IN,
    &IfxGtm_TIM0_6_TIN24_P33_2_IN,
    &IfxGtm_TIM0_7_TIN25_P33_3_IN
};

/* +1 = counted counts increase when the wheel drives the robot forward.
 * Mirrored gearboxes mean the raw signs differ per wheel; the values are
 * runtime-writable so the bench direction calibration (doc 23 section 8.4,
 * automated by the 0x70 pulse test in motor_algo.c) can flip a wheel without
 * a rebuild - ENCODER_setInvert() is the only writer, the ISR only reads. */
static sint8 g_encInvert[ENCODER_COUNT] = { +1, +1, +1, +1 };

/* Speed-conversion parameters, runtime state of the two macros above
 * (doc 34 SS8.2). Only the setters write them, always clamped legal, so the
 * 1 kHz conversions below can read them without re-checking. */
static sint32 g_fullScaleMmS = ENCODER_FULL_SCALE_MM_S;
static sint32 g_wheelDiaMm   = ENCODER_WHEEL_DIA_MM;

static volatile sint32  g_count[ENCODER_COUNT];   /* x4 decoded, signed        */
static volatile uint32 g_lastEdgeMs[ENCODER_COUNT];

/* published snapshot (ENCODER_task single writer) */
static sint32   g_speedMmS[2];
static uint32  g_odometerMm[2];
static boolean g_alive;

static boolean pinLevel(uint8 ch)
{
    return IfxPort_getPinState(g_tinPins[ch]->pin.port, g_tinPins[ch]->pin.pinIndex);
}

/* 4x quadrature: on an A edge count by B's level, on a B edge count by A's.
 * Levels are read after the edge has propagated, which is what the table
 * expects. */
static void enc_advance(uint8 ch, boolean edgeIsA)
{
    uint8  enc   = ch >> 1;
    uint8  other = edgeIsA ? (uint8)(ch + 1u) : (uint8)(ch - 1u);
    sint32 delta;

    if (edgeIsA)
    {
        boolean a = pinLevel(ch);
        boolean b = pinLevel(other);

        delta = a ? (b ? -1 : +1) : (b ? +1 : -1);
    }
    else
    {
        boolean a = pinLevel(other);
        boolean b = pinLevel(ch);

        delta = b ? (a ? +1 : -1) : (a ? -1 : +1);
    }

    g_count[enc] += delta * g_encInvert[enc];
    g_lastEdgeMs[enc] = STIME_nowMs();
}

IFX_INTERRUPT(enc0Isr, 0, ENC_ISR_PRIO_BASE + 0);
IFX_INTERRUPT(enc1Isr, 0, ENC_ISR_PRIO_BASE + 1);
IFX_INTERRUPT(enc2Isr, 0, ENC_ISR_PRIO_BASE + 2);
IFX_INTERRUPT(enc3Isr, 0, ENC_ISR_PRIO_BASE + 3);
IFX_INTERRUPT(enc4Isr, 0, ENC_ISR_PRIO_BASE + 4);
IFX_INTERRUPT(enc5Isr, 0, ENC_ISR_PRIO_BASE + 5);
IFX_INTERRUPT(enc6Isr, 0, ENC_ISR_PRIO_BASE + 6);
IFX_INTERRUPT(enc7Isr, 0, ENC_ISR_PRIO_BASE + 7);

void enc0Isr(void) { IfxGtm_Tim_Ch_clearNewValueEvent(IfxGtm_Tim_getChannel(&MODULE_GTM.TIM[ENC_TIM], IfxGtm_Tim_Ch_0)); enc_advance(0, TRUE);  }
void enc1Isr(void) { IfxGtm_Tim_Ch_clearNewValueEvent(IfxGtm_Tim_getChannel(&MODULE_GTM.TIM[ENC_TIM], IfxGtm_Tim_Ch_1)); enc_advance(1, FALSE); }
void enc2Isr(void) { IfxGtm_Tim_Ch_clearNewValueEvent(IfxGtm_Tim_getChannel(&MODULE_GTM.TIM[ENC_TIM], IfxGtm_Tim_Ch_2)); enc_advance(2, TRUE);  }
void enc3Isr(void) { IfxGtm_Tim_Ch_clearNewValueEvent(IfxGtm_Tim_getChannel(&MODULE_GTM.TIM[ENC_TIM], IfxGtm_Tim_Ch_3)); enc_advance(3, FALSE); }
void enc4Isr(void) { IfxGtm_Tim_Ch_clearNewValueEvent(IfxGtm_Tim_getChannel(&MODULE_GTM.TIM[ENC_TIM], IfxGtm_Tim_Ch_4)); enc_advance(4, TRUE);  }
void enc5Isr(void) { IfxGtm_Tim_Ch_clearNewValueEvent(IfxGtm_Tim_getChannel(&MODULE_GTM.TIM[ENC_TIM], IfxGtm_Tim_Ch_5)); enc_advance(5, FALSE); }
void enc6Isr(void) { IfxGtm_Tim_Ch_clearNewValueEvent(IfxGtm_Tim_getChannel(&MODULE_GTM.TIM[ENC_TIM], IfxGtm_Tim_Ch_6)); enc_advance(6, TRUE);  }
void enc7Isr(void) { IfxGtm_Tim_Ch_clearNewValueEvent(IfxGtm_Tim_getChannel(&MODULE_GTM.TIM[ENC_TIM], IfxGtm_Tim_Ch_7)); enc_advance(7, FALSE); }

static void enc_initChannel(uint8 ch, uint8 prio)
{
    Ifx_GTM_TIM_CH    *timCh = IfxGtm_Tim_getChannel(&MODULE_GTM.TIM[ENC_TIM], (IfxGtm_Tim_Ch)ch);
    volatile Ifx_SRC_SRCR *src;
    float32                filterClk;
    uint32                 deglitch;

    IfxGtm_PinMap_setTimTin((IfxGtm_Tim_TinMap *)g_tinPins[ch], IfxPort_InputMode_pullUp);

    /* Input event mode on both edges - same recipe as the iLLD IfxGtm_Tim_In
     * driver for IfxGtm_Tim_Mode_inputEvent + ActiveEdge_both: DSL=0, ISL=1. */
    timCh->CTRL.B.TIM_MODE = IfxGtm_Tim_Mode_inputEvent;
    timCh->CTRL.B.CLK_SEL  = IfxGtm_Cmu_Clk_0;
    timCh->CTRL.B.DSL      = 0u;
    timCh->CTRL.B.ISL      = 1u;
    timCh->CTRL.B.CNTS_SEL = IfxGtm_Tim_CntsSel_cntReg;
    timCh->CTRL.B.GPR0_SEL = IfxGtm_Tim_GprSel_cnts;
    timCh->CTRL.B.GPR1_SEL = IfxGtm_Tim_GprSel_cnts;
    timCh->CTRL.B.TIM_EN   = 1u;

    /* ~2 us deglitch on both edges; the Hall outputs are open drain pulled up
     * to 3V3, so edges are slower than push-pull (23-wiring.md section 8.1). */
    timCh->CTRL.B.FLT_EN       = 1u;
    timCh->CTRL.B.FLT_CNT_FRQ  = IfxGtm_Cmu_Tim_Filter_Clk_0;
    timCh->CTRL.B.FLT_MODE_RE  = IfxGtm_Tim_FilterMode_individualDeglitchTime;
    timCh->CTRL.B.FLT_MODE_FE  = IfxGtm_Tim_FilterMode_individualDeglitchTime;

    filterClk = IfxGtm_Tim_Ch_getFilterClockFrequency(&MODULE_GTM, timCh);
    deglitch  = (uint32)(filterClk * ENC_FILTER_TIME_S);
    if (deglitch > IFX_GTM_TIM_CH_FLT_RE_FLT_RE_MSK)
    {
        deglitch = IFX_GTM_TIM_CH_FLT_RE_FLT_RE_MSK;
    }

    timCh->FLT_RE.B.FLT_RE     = deglitch;
    timCh->FLT_FE.B.FLT_FE     = deglitch;

    /* Interrupt: one pulse per input edge, routed to CPU1. IN_SRC selects the
     * channel's own input pin (MODE=1, VAL=0); RMW so the 7 siblings stay. */
    IfxGtm_Tim_Ch_setNotificationMode(timCh, IfxGtm_IrqMode_pulseNotify);
    IfxGtm_Tim_Ch_setChannelNotification(timCh, TRUE, FALSE, FALSE, FALSE);
    IfxGtm_Tim_Ch_clearNewValueEvent(timCh);

    MODULE_GTM.TIM[ENC_TIM].IN_SRC.U |= (1u << (IFX_GTM_TIM_IN_SRC_MODE_0_OFF + (uint32)ch * 4u));

    src = IfxGtm_Tim_Ch_getSrcPointer(&MODULE_GTM, ENC_TIM, (IfxGtm_Tim_Ch)ch);
    IfxSrc_init(src, IfxSrc_Tos_cpu1, prio);
    IfxSrc_enable(src);
}

void ENCODER_init(void)
{
    uint8 i;

    /* GTM and CMU CLK0 are already up: MOTOR_init() runs first on this core. */
    for (i = 0u; i < 8u; i++)
    {
        enc_initChannel(i, ENC_ISR_PRIO_BASE + i);
    }

    for (i = 0u; i < ENCODER_COUNT; i++)
    {
        g_count[i]     = 0;
        g_lastEdgeMs[i] = 0u;
    }
    /* The record apply path can run before ENCODER_init on a cold core;
     * re-establish the defaults here so startup never inherits a stale
     * divisor. */
    g_fullScaleMmS = ENCODER_FULL_SCALE_MM_S;
    g_wheelDiaMm   = ENCODER_WHEEL_DIA_MM;
    g_speedMmS[0] = 0;
    g_speedMmS[1] = 0;
    g_odometerMm[0] = 0u;
    g_odometerMm[1] = 0u;
    g_alive = FALSE;
}

void ENCODER_task(void)
{
    static sint32   prev[ENCODER_COUNT];
    static sint32   winL[ENC_WINDOW_MS];
    static sint32   winR[ENC_WINDOW_MS];
    static uint8   winIdx = 0u;
    static boolean winFull = FALSE;
    static float32 odomAcc[2];
    static uint32  lastMoveMs;
    static boolean firstCall = TRUE;

    sint32  d[ENCODER_COUNT];
    sint32  dl, dr;
    uint32 now = STIME_nowMs();
    uint8  i;

    for (i = 0u; i < ENCODER_COUNT; i++)
    {
        d[i] = g_count[i] - prev[i];
        prev[i] = g_count[i];
    }
    dl = d[ENCODER_SIDE_LEFT_0] + d[ENCODER_SIDE_LEFT_1];
    dr = d[ENCODER_SIDE_RIGHT_0] + d[ENCODER_SIDE_RIGHT_1];

    if (firstCall)
    {
        firstCall = FALSE;
        lastMoveMs = now;
    }

    if ((dl != 0) || (dr != 0))
    {
        lastMoveMs = now;
    }
    g_alive = (uint32)(now - lastMoveMs) < ENC_ALIVE_WINDOW_MS;

    winL[winIdx] = dl;
    winR[winIdx] = dr;
    winIdx = (uint8)((winIdx + 1u) % ENC_WINDOW_MS);
    if (winIdx == 0u)
    {
        winFull = TRUE;
    }

    {
        /* Window MEAN, not median (doc 21 section 5.1, V1.13). One side moves
         * ~14 counts/ms at 1 m/s, i.e. 1 count/ms = 71 mm/s: a median of
         * integer per-ms deltas is quantised to 71 mm/s steps and returns 0
         * whenever most 1 ms slots are empty - every speed below ~70 mm/s,
         * and a lot of the ramp, read "0" while the wheels visibly turn.
         * The sum over the 8 ms window keeps every count (8.9 mm/s
         * resolution). The median bought nothing here: the x4 software
         * decode can only produce +-1 per real edge, and a glitch pair
         * (+1/-1) cancels in a sum anyway. */
        uint8  n    = winFull ? ENC_WINDOW_MS : winIdx;
        uint8  j;
        sint32 sumL = 0, sumR = 0;

        for (j = 0u; j < n; j++)
        {
            sumL += winL[j];
            sumR += winR[j];
        }

        /* side delta counts BOTH wheels of the side, so a wheel rev is
         * 2 * ENCODER_COUNTS_WHEEL_REV; side counts/ms -> wheel mm/s.
         * circMm/mmPerCount ride the runtime wheel diameter (doc 34 SS8.2,
         * setter-clamped); the formulas are the pre-existing ones, fed the
         * window mean counts/ms instead of the median. */
        if (n > 0u)
        {
            float32 countsPerWheelRev = (float32)(2u * ENCODER_COUNTS_WHEEL_REV);
            float32 circMm            = 3.14159265f * (float32)g_wheelDiaMm;
            float32 meanL             = (float32)sumL / (float32)n;
            float32 meanR             = (float32)sumR / (float32)n;
            float32 mmPerS            = (meanL * 1000.0f / countsPerWheelRev) * circMm;

            g_speedMmS[0] = (sint32)mmPerS;
            mmPerS        = (meanR * 1000.0f / countsPerWheelRev) * circMm;
            g_speedMmS[1] = (sint32)mmPerS;
        }

        odomAcc[0] += (dl >= 0) ? (float32)dl : (float32)(-dl);
        odomAcc[1] += (dr >= 0) ? (float32)dr : (float32)(-dr);
        {
            float32 mmPerCount = (float32)g_wheelDiaMm * 3.14159265f
                               / (float32)(2u * ENCODER_COUNTS_WHEEL_REV);

            g_odometerMm[0] = (uint32)(odomAcc[0] * mmPerCount);
            g_odometerMm[1] = (uint32)(odomAcc[1] * mmPerCount);
        }
    }

    ENCODER_publish();
}

void ENCODER_getSpeedsMmS(sint32 v[2])
{
    v[0] = g_speedMmS[0];
    v[1] = g_speedMmS[1];
}

void ENCODER_getRawCounts(sint32 c[4])
{
    uint8 i;

    for (i = 0u; i < ENCODER_COUNT; i++)
    {
        c[i] = g_count[i];
    }
}

void ENCODER_getOdometer(uint32 m[2])
{
    m[0] = g_odometerMm[0];
    m[1] = g_odometerMm[1];
}

boolean ENCODER_isAlive(void)
{
    return g_alive;
}

/* Bench direction calibration entry (doc 23 section 8.4 step 2). sign is
 * clamped to +-1; 0 is refused - "no direction" is not a calibration, it is
 * the bug this call exists to fix. Called from the motor_algo calibration
 * task (CPU1, same core as the ISRs), never from another core. */
void ENCODER_setInvert(uint8 enc, sint8 sign)
{
    if ((enc >= ENCODER_COUNT) || (sign == 0))
    {
        return;
    }
    g_encInvert[enc] = (sign > 0) ? (sint8)+1 : (sint8)-1;
}

sint8 ENCODER_getInvert(uint8 enc)
{
    return (enc < ENCODER_COUNT) ? g_encInvert[enc] : (sint8)0;
}

/* Setters clamp to the doc 34 SS8.1 ranges; an illegal value (a DFlash bit
 * flip reaching the apply path) falls back to the macro default instead of
 * dividing by zero or scaling by nonsense in the 1 kHz loop. */
void ENCODER_setFullScaleMmS(sint32 mmS)
{
    g_fullScaleMmS = (mmS >= CALIB_FULLSCALE_MIN) && (mmS <= CALIB_FULLSCALE_MAX)
                         ? mmS : (sint32)ENCODER_FULL_SCALE_MM_S;
}

sint32 ENCODER_getFullScaleMmS(void)
{
    return g_fullScaleMmS;
}

void ENCODER_setWheelDiaMm(sint32 mm)
{
    g_wheelDiaMm = (mm >= CALIB_WHEELDIA_MIN) && (mm <= CALIB_WHEELDIA_MAX)
                       ? mm : (sint32)ENCODER_WHEEL_DIA_MM;
}

sint32 ENCODER_getWheelDiaMm(void)
{
    return g_wheelDiaMm;
}

/* The mm/s figures ride a sint16 across xcore; the physical top speed of this
 * drivetrain is two orders below the limit, so saturation only trips when the
 * count source is broken - clamp instead of wrapping into a bogus sign. */
static sint16 enc_satS16(sint32 v)
{
    if (v > 32767)
    {
        return 32767;
    }
    if (v < -32768)
    {
        return -32768;
    }
    return (sint16)v;
}

/* Publish both unit domains in one snapshot: percent*10 for the demo status
 * overlay (Cpu0_Main) and physical mm/s + per-side odometer for the SF
 * telemetry fields vMeasL/R and odoSession (SDD §6.3, fed by Cpu2_Main). */
void ENCODER_publish(void)
{
    XcoreEncoder enc;

    enc.pctLeft  = (sint16)((g_speedMmS[0] * 1000) / g_fullScaleMmS);
    enc.pctRight = (sint16)((g_speedMmS[1] * 1000) / g_fullScaleMmS);

    if (enc.pctLeft > 1000)  { enc.pctLeft = 1000;  }
    if (enc.pctLeft < -1000) { enc.pctLeft = -1000; }
    if (enc.pctRight > 1000)  { enc.pctRight = 1000;  }
    if (enc.pctRight < -1000) { enc.pctRight = -1000; }

    enc.vMeasLeftMmS  = enc_satS16(g_speedMmS[0]);
    enc.vMeasRightMmS = enc_satS16(g_speedMmS[1]);
    enc.odoLeftMm     = g_odometerMm[0];
    enc.odoRightMm    = g_odometerMm[1];
    enc.alive         = g_alive;
    ENCODER_getRawCounts(enc.raw);

    XCORE_encoderPublish(&enc);
}
