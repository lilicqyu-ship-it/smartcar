#include "motor.h"

#include "IfxGtm_Atom_Pwm.h"
#include "IfxGtm_PinMap.h"
#include "IfxPort.h"

#define MOTOR_PWM_FREQUENCY  20000U

typedef struct
{
    IfxGtm_Atom_Pwm_Driver pwm;
    uint32                 period;
    IfxPort_Pin            dir1;   /* TB6612 IN1 */
    IfxPort_Pin            dir2;   /* TB6612 IN2 */
} Motor_t;

static Motor_t g_motors[MOTOR_COUNT];

static const IfxGtm_Atom_ToutMap *const g_pwmPins[MOTOR_COUNT] = {
    &IfxGtm_ATOM2_4_TOUT51_P21_0_OUT,   /* Motor A: PWMA P21.0  */
    &IfxGtm_ATOM4_1_TOUT54_P21_3_OUT,   /* Motor B: PWMB P21.3  */
    &IfxGtm_ATOM1_0_TOUT9_P00_0_OUT,    /* Motor C: PWMA P00.0  */
    &IfxGtm_ATOM0_7_TOUT17_P00_8_OUT    /* Motor D: PWMB P00.8  */
};

static const IfxPort_Pin g_dirPins[MOTOR_COUNT][2] = {
    { {&MODULE_P21, 4},  {&MODULE_P21, 5}   },  /* Motor A: AIN1 P21.4, AIN2 P21.5  */
    { {&MODULE_P21, 2},  {&MODULE_P22, 3}   },  /* Motor B: BIN1 P21.2, BIN2 P22.3  */
    { {&MODULE_P00, 2},  {&MODULE_P00, 6}   },  /* Motor C: AIN1 P00.2, AIN2 P00.6  */
    { {&MODULE_P00, 10}, {&MODULE_P00, 12}  }   /* Motor D: BIN1 P00.10, BIN2 P00.12 */
};

/* Direction invert: 1 = swap IN1/IN2 so "forward" matches the other wheels.
 * Wheel layout: A=front, D=front, B=rear, C=rear; A/B on one side, C/D on the
 * other side (mirrored mounting). Verified: A/D same dir, B/C same dir -> B/C inverted. */
static const boolean g_dirInvert[MOTOR_COUNT] = {
    FALSE,   /* Motor A (front)  */
    TRUE,    /* Motor B (rear)   */
    TRUE,    /* Motor C (rear)   */
    FALSE    /* Motor D (front)  */
};

static void MOTOR_setDuty(MotorId id, uint32 dutyTicks)
{
    Motor_t *m = &g_motors[id];

    IfxGtm_Atom_Ch_setCompareOneShadow(m->pwm.atom, m->pwm.atomChannel, dutyTicks);
    IfxGtm_Atom_Agc_trigger(m->pwm.agc);
}

static void MOTOR_setDir(MotorId id, boolean forward)
{
    Motor_t *m = &g_motors[id];

    forward = forward ^ g_dirInvert[id];

    IfxPort_setPinState(m->dir1.port, m->dir1.pinIndex, forward ? IfxPort_State_high : IfxPort_State_low);
    IfxPort_setPinState(m->dir2.port, m->dir2.pinIndex, forward ? IfxPort_State_low : IfxPort_State_high);
}

void MOTOR_init(void)
{
    float32 gtmFreq;

    IfxGtm_enable(&MODULE_GTM);
    gtmFreq = IfxGtm_Cmu_getModuleFrequency(&MODULE_GTM);
    IfxGtm_Cmu_setGclkFrequency(&MODULE_GTM, gtmFreq);
    IfxGtm_Cmu_setClkFrequency(&MODULE_GTM, IfxGtm_Cmu_Clk_0, gtmFreq);
    IfxGtm_Cmu_enableClocks(&MODULE_GTM, IFXGTM_CMU_CLKEN_FXCLK | IFXGTM_CMU_CLKEN_CLK0);

    {
        MotorId i;

        for (i = 0; i < MOTOR_COUNT; i++)
        {
            IfxGtm_Atom_Pwm_Config cfg;

            IfxGtm_Atom_Pwm_initConfig(&cfg, &MODULE_GTM);
            cfg.atom                      = g_pwmPins[i]->atom;
            cfg.atomChannel               = g_pwmPins[i]->channel;
            cfg.synchronousUpdateEnabled  = TRUE;
            cfg.immediateStartEnabled     = TRUE;
            cfg.pin.outputPin             = g_pwmPins[i];
            cfg.pin.outputMode            = IfxPort_OutputMode_pushPull;
            cfg.pin.padDriver             = IfxPort_PadDriver_cmosAutomotiveSpeed1;

            IfxGtm_Atom_Ch_setClockSource(&MODULE_GTM.ATOM[cfg.atom], cfg.atomChannel, IfxGtm_Cmu_Clk_0);

            cfg.period  = (uint32)(gtmFreq / MOTOR_PWM_FREQUENCY);
            cfg.dutyCycle = 0;

            IfxGtm_Atom_Pwm_init(&g_motors[i].pwm, &cfg);
            g_motors[i].period = cfg.period;

            g_motors[i].dir1 = g_dirPins[i][0];
            g_motors[i].dir2 = g_dirPins[i][1];

            IfxPort_setPinMode(g_dirPins[i][0].port, g_dirPins[i][0].pinIndex, IfxPort_Mode_outputPushPullGeneral);
            IfxPort_setPinMode(g_dirPins[i][1].port, g_dirPins[i][1].pinIndex, IfxPort_Mode_outputPushPullGeneral);
            IfxPort_setPinLow(g_dirPins[i][0].port, g_dirPins[i][0].pinIndex);
            IfxPort_setPinLow(g_dirPins[i][1].port, g_dirPins[i][1].pinIndex);
        }
    }
}

void MOTOR_setSpeed(MotorId id, sint16 speed)
{
    uint32 dutyTicks;

    if (id >= MOTOR_COUNT)
    {
        return;
    }

    if (speed > 1000)
    {
        speed = 1000;
    }
    else if (speed < -1000)
    {
        speed = -1000;
    }

    if (speed == 0)
    {
        MOTOR_stop(id);
        return;
    }

    MOTOR_setDir(id, speed > 0);
    dutyTicks = (uint32)((uint32)((speed > 0) ? speed : -speed) * g_motors[id].period / 1000U);
    MOTOR_setDuty(id, dutyTicks);
}

void MOTOR_brake(MotorId id)
{
    Motor_t *m = &g_motors[id];

    IfxPort_setPinHigh(m->dir1.port, m->dir1.pinIndex);
    IfxPort_setPinHigh(m->dir2.port, m->dir2.pinIndex);
    MOTOR_setDuty(id, 0);
}

void MOTOR_stop(MotorId id)
{
    Motor_t *m = &g_motors[id];

    IfxPort_setPinLow(m->dir1.port, m->dir1.pinIndex);
    IfxPort_setPinLow(m->dir2.port, m->dir2.pinIndex);
    MOTOR_setDuty(id, 0);
}

void MOTOR_stopAll(void)
{
    MotorId i;

    for (i = 0; i < MOTOR_COUNT; i++)
    {
        MOTOR_stop(i);
    }
}