/*
 * test_adc.c - host unit tests for the battery-VIN conversion math
 *
 * bsp/adc.c needs the VADC, but the counts -> pin mV -> VIN mV chain is pure
 * math living inline in bsp/adc.h - these tests compile the exact helpers the
 * firmware runs (stub Ifx_Types.h stands in for the iLLD base types). The EMA
 * filter and the driver itself are hardware-bound and not covered here.
 *
 * Regression guard: ADC_BATT_DIV_NUM is stored x1000 and divided by 1000 at
 * the helper. Defining it as a bare 11 once made g_vinMv carry whole volts
 * while telemetry/UI consume mV - a 12.6 V battery read as "0.01 V", 0%.
 *
 * Build & run (MSYS2/MinGW host), same pattern as test_sf.c:
 *   gcc -std=c99 -Wall -Wextra -Werror -O2 -I . -I test/host/stub \
 *       test/host/test_adc.c -o test/host/out/test_adc && test/host/out/test_adc
 */
#include <stdio.h>

#include "bsp/adc.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond))                                                      \
        {                                                                 \
            g_failed++;                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        long _a = (long)(a), _b = (long)(b);                              \
        g_checks++;                                                       \
        if (_a != _b)                                                     \
        {                                                                 \
            g_failed++;                                                   \
            printf("FAIL %s:%d  %s == %s  (%ld vs %ld)\n",                \
                   __FILE__, __LINE__, #a, #b, _a, _b);                   \
        }                                                                 \
    } while (0)

/* counts -> divider-node mV: 4095 counts is by definition the VAREF rail */
static void test_counts_to_pin_mv(void)
{
    CHECK_EQ(ADC_countsToPinMv(0), 0);
    CHECK_EQ(ADC_countsToPinMv(4095), 3300);
    CHECK_EQ(ADC_countsToPinMv(1365), 1100);   /* exact third       */
    CHECK_EQ(ADC_countsToPinMv(2730), 2200);   /* exact two thirds  */
    CHECK_EQ(ADC_countsToPinMv(732), 589);     /* 6.5 V on the divider, 1 mV truncation */
}

/* pin mV -> VIN mV: x1000 convention, exactly 11.000x - no rounding on top
 * of the pin-mV truncation (11000/1000 divides evenly) */
static void test_pin_mv_to_vin_mv(void)
{
    CHECK_EQ(ADC_pinMvToVinMv(0), 0);
    CHECK_EQ(ADC_pinMvToVinMv(590), 6490);     /* 2S low,  6.5 V */
    CHECK_EQ(ADC_pinMvToVinMv(1100), 12100);
    CHECK_EQ(ADC_pinMvToVinMv(1145), 12595);   /* 3S top, 12.6 V - was 12 with the bare-11 mixup */
    CHECK_EQ(ADC_pinMvToVinMv(3300), 36300);   /* full-scale pin, must still fit uint16 */
}

/* whole chain at the bench anchors of doc/20-design/23-wiring.md 5.5 */
static void test_full_chain(void)
{
    CHECK_EQ(ADC_pinMvToVinMv(ADC_countsToPinMv(1421)), 12595); /* 12.6 V  */
    CHECK_EQ(ADC_pinMvToVinMv(ADC_countsToPinMv(732)), 6479);   /* 6.5 V   */
    CHECK_EQ(ADC_pinMvToVinMv(ADC_countsToPinMv(4095)), 36300); /* ceiling */
}

int main(void)
{
    test_counts_to_pin_mv();
    test_pin_mv_to_vin_mv();
    test_full_chain();

    printf("test_adc: %d checks, %d failed\n", g_checks, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
