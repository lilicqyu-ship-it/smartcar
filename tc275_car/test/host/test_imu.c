/*
 * test_imu.c - host unit tests for the LSM6DSV16BX conversion math
 *
 * bsp/imu.c needs QSPI1, the SCU ERU and the sensor, but the unit
 * conversions and the SPI wire-byte builders are pure inline functions in
 * bsp/imu.h - these tests compile the exact helpers the firmware runs (the
 * stub Ifx_Types.h stands in for the iLLD base types). The driver state
 * machine, the ERU setup and the burst framing are hardware-bound and not
 * covered here (bench: doc 23 section 10.2 first checks + doc 35 section 8).
 *
 * Regression anchors: the sensitivity constants are the official ST driver's
 * (stm32duino LSM6DSV16X): 0.061/0.122/0.244/0.488 mg/LSB and
 * 4.375 = 35/8 mdps/LSB at 125 dps - the num/den split must stay exact.
 * The DSV16X register layout (ODR in the LOW nibble of CTRL1/2, gyro FS in
 * CTRL6 as a 4-bit field with 4000 dps = 0xC) is what the ctrl byte builders
 * lock in.
 *
 * Build & run (MSYS2/MinGW host), same pattern as test_adc.c:
 *   gcc -std=c99 -Wall -Wextra -Werror -O2 -I . -I test/host/stub \
 *       test/host/test_imu.c -o test/host/out/test_imu && test/host/out/test_imu
 */
#include <stdio.h>

#include "bsp/imu.h"

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

/* symmetric rounding, half away from zero */
static void test_div_round(void)
{
    CHECK_EQ(IMU_divRound(0, 1000), 0);
    CHECK_EQ(IMU_divRound(4, 2), 2);        /* exact                            */
    CHECK_EQ(IMU_divRound(-4, 2), -2);      /* exact, no positive bias          */
    CHECK_EQ(IMU_divRound(5, 2), 3);        /* 2.5 -> 3, away from zero         */
    CHECK_EQ(IMU_divRound(-5, 2), -3);      /* -2.5 -> -3, not -2               */
    CHECK_EQ(IMU_divRound(3, 2), 2);        /* 1.5 -> 2                         */
    CHECK_EQ(IMU_divRound(-3, 2), -2);
    CHECK_EQ(IMU_divRound(1499, 1000), 1);  /* 1.499 -> 1                       */
    CHECK_EQ(IMU_divRound(1500, 1000), 2);  /* 1.5 -> 2                         */
}

/* first wire byte: bit7 = R/W (1 = read), 7-bit register address */
static void test_addr_bytes(void)
{
    CHECK_EQ(IMU_readAddrByte(IMU_REG_WHO_AM_I), 0x8F);
    CHECK_EQ(IMU_writeAddrByte(IMU_REG_WHO_AM_I), 0x0F);
    CHECK_EQ(IMU_readAddrByte(IMU_REG_OUT_TEMP_L), 0xA0);
    CHECK_EQ(IMU_writeAddrByte(IMU_REG_OUTX_L_A), 0x28);
    /* address is masked to 7 bits - a caller bug must not set the R/W bit */
    CHECK_EQ(IMU_writeAddrByte(0xFF), 0x7F);
}

/* CTRL1/CTRL2 = ODR in the low nibble (DSV16X layout, reversed vs LSM6DSL),
 * CTRL3 = IF_INC|BDU = 0x44 */
static void test_ctrl_bytes(void)
{
    CHECK_EQ(IMU_ctrlOdrByte(IMU_ODR_240HZ), 0x07);
    CHECK_EQ(IMU_ctrlOdrByte(IMU_ODR_120HZ), 0x06);
    CHECK_EQ(IMU_ctrl3Byte(), 0x44);
}

/* accel: mg = raw * num[fs] / 1000, num = {61,122,244,488} */
static void test_accel_conversions(void)
{
    /* +-4 g (the production default, num 122): 8192 LSB = 999.4 mg */
    CHECK_EQ(IMU_rawToMilliG(0, IMU_XL_FS_4G), 0);
    CHECK_EQ(IMU_rawToMilliG(8192, IMU_XL_FS_4G), 999);
    CHECK_EQ(IMU_rawToMilliG(-8192, IMU_XL_FS_4G), -999);
    CHECK_EQ(IMU_rawToMilliG(16384, IMU_XL_FS_4G), 1999);   /* ~2 g          */
    CHECK_EQ(IMU_rawToMilliG(32767, IMU_XL_FS_4G), 3998);   /* near +FS      */
    CHECK_EQ(IMU_rawToMilliG(-32768, IMU_XL_FS_4G), -3998);

    /* +-2 g: 16384 LSB = 999.4 mg again (0.061 * 16384) */
    CHECK_EQ(IMU_rawToMilliG(16384, IMU_XL_FS_2G), 999);
    CHECK_EQ(IMU_rawToMilliG(1, IMU_XL_FS_2G), 0);          /* 0.061 -> 0    */
    CHECK_EQ(IMU_rawToMilliG(-16384, IMU_XL_FS_2G), -999);

    /* +-16 g ceiling must stay inside the sint16 mg domain: ~15990 */
    CHECK_EQ(IMU_rawToMilliG(32767, IMU_XL_FS_16G), 15990);

    /* illegal code clamps to the default +-4 g, never indexes out of range */
    CHECK_EQ(IMU_rawToMilliG(8192, 7), 999);
}

/* gyro: mdps = raw * 35 / {8,4,2,1} (125..1000 dps), * 70 / * 140 above */
static void test_gyro_conversions(void)
{
    /* +-500 dps (the production default, 35/2): 1000 LSB = 175.00 dps */
    CHECK_EQ(IMU_rawToMilliDps(0, IMU_GY_FS_500DPS), 0);
    CHECK_EQ(IMU_rawToMilliDps(1000, IMU_GY_FS_500DPS), 17500);
    CHECK_EQ(IMU_rawToMilliDps(-1000, IMU_GY_FS_500DPS), -17500);
    CHECK_EQ(IMU_rawToMilliDps(572, IMU_GY_FS_500DPS), 10010); /* exact     */
    CHECK_EQ(IMU_rawToMilliDps(32767, IMU_GY_FS_500DPS), 573423); /* 0.5 dps below +FS, rounds up */

    /* +-125 dps, the 35/8 pair: 8 LSB = exactly 35 mdps */
    CHECK_EQ(IMU_rawToMilliDps(8, IMU_GY_FS_125DPS), 35);
    CHECK_EQ(IMU_rawToMilliDps(1, IMU_GY_FS_125DPS), 4);    /* 4.375 -> 4    */
    CHECK_EQ(IMU_rawToMilliDps(-1, IMU_GY_FS_125DPS), -4);
    CHECK_EQ(IMU_rawToMilliDps(2, IMU_GY_FS_125DPS), 9);    /* 8.75 -> 9     */

    /* +-250 and +-1000: 35/4 and 35/1 */
    CHECK_EQ(IMU_rawToMilliDps(4000, IMU_GY_FS_250DPS), 35000);
    CHECK_EQ(IMU_rawToMilliDps(1000, IMU_GY_FS_1000DPS), 35000);

    /* +-2000 / +-4000 dps */
    CHECK_EQ(IMU_rawToMilliDps(468, IMU_GY_FS_2000DPS), 32760);
    CHECK_EQ(IMU_rawToMilliDps(100, IMU_GY_FS_4000DPS), 14000);

    /* illegal code clamps to +-500 dps */
    CHECK_EQ(IMU_rawToMilliDps(1000, 5), 17500);
}

/* temperature: centiC = 2500 + raw * 100 / 256 (25.00 degC at raw 0) */
static void test_temp_conversion(void)
{
    CHECK_EQ(IMU_rawToCentiC(0), 2500);
    CHECK_EQ(IMU_rawToCentiC(2560), 3500);   /* +10 degC                     */
    CHECK_EQ(IMU_rawToCentiC(256), 2600);
    CHECK_EQ(IMU_rawToCentiC(128), 2550);    /* +0.5 degC, exact             */
    CHECK_EQ(IMU_rawToCentiC(64), 2525);     /* +0.25 degC, exact            */
    CHECK_EQ(IMU_rawToCentiC(-512), 2300);   /* below room, no positive bias */
}

/* FS code -> table row mapping, including the out-of-order 0xC entry */
static void test_fs_index_mapping(void)
{
    CHECK_EQ(IMU_xlFsIndex(IMU_XL_FS_2G), 0);
    CHECK_EQ(IMU_xlFsIndex(IMU_XL_FS_16G), 3);
    CHECK_EQ(IMU_xlFsIndex(9), 1);          /* clamp to default +-4 g        */

    CHECK_EQ(IMU_gyFsIndex(IMU_GY_FS_125DPS), 0);
    CHECK_EQ(IMU_gyFsIndex(IMU_GY_FS_2000DPS), 4);
    CHECK_EQ(IMU_gyFsIndex(IMU_GY_FS_4000DPS), 5);   /* 0xC sits at row 5    */
    CHECK_EQ(IMU_gyFsIndex(5), 2);          /* clamp to default +-500 dps    */
}

int main(void)
{
    test_div_round();
    test_addr_bytes();
    test_ctrl_bytes();
    test_accel_conversions();
    test_gyro_conversions();
    test_temp_conversion();
    test_fs_index_mapping();

    printf("test_imu: %d checks, %d failed\n", g_checks, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
