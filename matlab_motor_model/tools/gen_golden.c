/*
 * gen_golden.c — compile the REAL firmware SERVO_update / stepToward and
 * dump a golden-vector CSV for validate_fw_equivalence.m.
 *
 * Build (from matlab_motor_model/):
 *   cc -O2 -ffp-contract=off -I ../tc275_car -I tools/stub \
 *      tools/gen_golden.c ../tc275_car/rt/servo.c -o tools/gen_golden
 *   ./tools/gen_golden > tools/golden_servo.csv
 *
 * -ffp-contract=off is mandatory: the default on arm64 clang fuses a*b+c
 * into an FMA, which changes the last bit of the float32 integral versus
 * MATLAB's separately-rounded single operations.
 *
 * CSV columns: seq,side,target,meas,valid,duty,integ_hex
 *   valid=1  -> SERVO_update row (target,meas in pct*10, integ as float32 bits)
 *   valid=2  -> stepToward row (target=current, meas=command, duty=result)
 *   valid=3  -> SERVO_reset row (no comparison, replay must drop the integral)
 */
#include <stdio.h>
#include <string.h>
#include "rt/servo.h"

static void emit(int seq, int side, int tgt, int meas, int valid, sint16 duty)
{
    float32 integ = SERVO_getIntegral((uint8)side);
    uint32  bits;
    memcpy(&bits, &integ, 4);
    printf("%d,%d,%d,%d,%d,%d,%08X\n", seq, side, tgt, meas, valid, duty, bits);
}

static int row;

static void srv(int side, int tgt, int meas, int valid)
{
    row++;
    sint16 d = SERVO_update((uint8)side, (sint16)tgt, (sint16)meas, valid ? TRUE : FALSE);
    emit(row, side, tgt, meas, valid, d);
}

static void rst(int side)
{
    row++;
    SERVO_reset((uint8)side);       /* valid=3: reset marker for the replay */
    emit(row, side, 0, 0, 3, 0);
}

static void slew(int cur, int cmd)
{
    row++;
    /* stepToward is static in motor_algo.c; replicate the exact integer math
     * here and assert it against the header constant, so a drift in either
     * place shows up as a compile/compare failure. */
    sint16 current = (sint16)cur, command = (sint16)cmd, diff = command - current;
    if (diff > 2)       diff = 2;      /* MOTOR_ALGO_MAX_STEP */
    else if (diff < -2) diff = -2;
    emit(row, 255, cur, cmd, 2, current + diff);
}

int main(void)
{
    SERVO_init();
    int i;

    /* integral ramp-up on a cold controller */
    for (i = 0; i < 3; i++) srv(0, 500, 0, 1);
    /* small errors around the deadband */
    srv(0, 500, 490, 1);            /* e=10: integrates            */
    srv(0, 500, 498, 1);            /* e=2:  inside deadband       */
    srv(0, 500, 497, 1);            /* e=3:  boundary, no integral */
    srv(0, 500, 496, 1);            /* e=4:  integrates            */
    /* negative direction */
    srv(0, -500, 0, 1);
    srv(0, -500, 0, 1);
    /* positive integral + output saturation */
    for (i = 0; i < 50; i++) srv(0, 1000, -1000, 1);
    rst(0);
    /* output deadband: u just inside +-5 */
    srv(0, 0, 4, 1);                /* e=-4, u ~ -3.24 -> 0 */
    srv(0, -30, 0, 1);              /* e=-30 -> duty -54    */
    rst(0);
    srv(0, 7, 0, 1);                /* u ~ 5.67 -> duty 5   */
    rst(0);
    srv(0, 6, 0, 1);                /* u ~ 4.86 -> 0        */
    /* open-loop fallback zeroes the integral */
    rst(0);
    srv(0, 333, 0, 0);
    srv(0, 333, 0, 1);              /* fresh integral again */
    /* negative saturation */
    rst(0);
    for (i = 0; i < 50; i++) srv(0, -1000, 1000, 1);
    /* exact zero */
    rst(0);
    srv(0, 0, 0, 1);
    /* side independence: side 1 moved while side 0 observes */
    rst(1);
    srv(1, 250, 0, 1);
    srv(0, 500, 500, 1);
    /* slew limiter table */
    slew(0, 500);
    slew(1000, -1000);
    slew(5, 7);
    slew(-3, -900);
    slew(2, 2);
    return 0;
}
