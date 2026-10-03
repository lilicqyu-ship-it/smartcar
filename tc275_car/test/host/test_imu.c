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
 * Regression anchors: the sensitivity constants come from ST's official PID
 * driver (github.com/STMicroelectronics/lsm6dsv16bx-pid, lsm6dsv16bx_reg.h/.c):
 * 0.061/0.122/0.244/0.488 mg/LSB and 4.375 = 35/8 mdps/LSB at 125 dps - the
 * num/den split must stay exact. The DSV16X register layout (ODR in the LOW
 * nibble of CTRL1/2, gyro FS in CTRL6 as a 4-bit field with 4000 dps = 0xC) is
 * what the ctrl byte builders lock in.
 *
 * The identity and the burst layout are locked here too, because both were
 * wrong once: WHO_AM_I is 0x71 (not the 0x70 of a different part), the 16-bit
 * samples are little-endian (low address = LSB, official
 * "buff[0] | (buff[1] << 8)"), and the 0x20 burst is temp -> gyro X,Y,Z ->
 * accel Z,Y,X, i.e. the accel axis order is reversed relative to the gyro one.
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

/* The identity the whole probe hangs on: datasheet 0Fh = 71h, and
 * LSM6DSV16BX_ID in ST's driver. 0x70 belongs to a different part and made a
 * healthy, correctly wired sensor read as absent. */
static void test_who_am_i(void)
{
    CHECK_EQ(IMU_WHO_AM_I_VAL, 0x71);
}

/* 16-bit output registers are little-endian: the LOW address holds the LSB
 * (official "buff[0] | (buff[1] << 8)", and d[0] is buff[0] here because the
 * burst starts at OUT_TEMP_L). The sign must come from the high byte. */
static void test_le16(void)
{
    CHECK_EQ(IMU_le16(0x00, 0x00), 0);
    CHECK_EQ(IMU_le16(0x01, 0x00), 1);         /* 1 LSB, no high byte        */
    CHECK_EQ(IMU_le16(0x34, 0x12), 0x1234);    /* low address = low byte     */
    CHECK_EQ(IMU_le16(0xFF, 0x7F), 32767);     /* +FS                        */
    CHECK_EQ(IMU_le16(0x00, 0x80), -32768);    /* -FS, sign in the HIGH byte */
    CHECK_EQ(IMU_le16(0x18, 0xFC), -1000);     /* two's complement round trip */
}

/* Burst decode: every axis gets a distinct byte pair, so neither a swapped
 * pair (endianness) nor a swapped axis can pass. Then the same buffer is
 * converted to engineering units, which is what the iOS map and the V2.0
 * fusion actually consume. */
static void test_burst_decode(void)
{
    const uint8 d[14] = {
        0x00u, 0x64u,          /* temp  0x20 / 0x21                           */
        0x11u, 0x22u,          /* gyro X 0x22 / 0x23                          */
        0x33u, 0x44u,          /* gyro Y 0x24 / 0x25                          */
        0x55u, 0x66u,          /* gyro Z 0x26 / 0x27                          */
        0x77u, 0x88u,          /* accel Z 0x28 / 0x29                         */
        0x99u, 0xAAu,          /* accel Y 0x2A / 0x2B                         */
        0xBBu, 0xCCu           /* accel X 0x2C / 0x2D                         */
    };

    CHECK_EQ(IMU_burstTempRaw(d), 0x6400);
    CHECK_EQ(IMU_burstGyroRaw(d, 0u), 0x2211);
    CHECK_EQ(IMU_burstGyroRaw(d, 1u), 0x4433);
    CHECK_EQ(IMU_burstGyroRaw(d, 2u), 0x6655);
    /* The accel block is declared Z,Y,X, so X is the LAST pair and Z the first
     * one: reading it in declaration order is what put gravity on axis Z. */
    CHECK_EQ(IMU_burstAccRaw(d, 0u), (sint16)0xCCBB);
    CHECK_EQ(IMU_burstAccRaw(d, 1u), (sint16)0xAA99);
    CHECK_EQ(IMU_burstAccRaw(d, 2u), (sint16)0x8877);

    /* Flat on its back: +1 g on X, a slow clockwise yaw, die at 24.00 degC. */
    {
        uint8 b[14] = { 0u };

        b[0]  = 0x00u; b[1]  = 0xFFu;   /* temp raw -256                       */
        b[4]  = 0x18u; b[5]  = 0xFCu;   /* gyro Y raw -1000                    */
        b[12] = 0x00u; b[13] = 0x20u;   /* accel X raw +8192                   */

        CHECK_EQ(IMU_rawToCentiC(IMU_burstTempRaw(b)), 2400);
        CHECK_EQ(IMU_rawToMilliDps(IMU_burstGyroRaw(b, 1u), IMU_GY_FS_500DPS), -17500);
        CHECK_EQ(IMU_rawToMilliG(IMU_burstAccRaw(b, 0u), IMU_CFG_XL_FS), 999);
        /* Gravity must stay off the other axes, and the other gyro axes at 0:
         * a byte-order or accel-order slip moves these first. */
        CHECK_EQ(IMU_rawToMilliG(IMU_burstAccRaw(b, 2u), IMU_CFG_XL_FS), 0);
        CHECK_EQ(IMU_rawToMilliDps(IMU_burstGyroRaw(b, 0u), IMU_GY_FS_500DPS), 0);
        CHECK_EQ(IMU_rawToCentiC(IMU_burstTempRaw(d)), 12500);  /* raw 25600 = +100 degC */
    }
}

/* Blank-burst detection: the bench signature of a module that browned out and
 * reset itself (or lost power outright) is 14 x 0x00 on a transaction the QSPI
 * reports as clean, with OUT_TEMP raw 0 - i.e. the IMU row reads
 * "0 0 0 0 0 0 2500" forever. 0xFF is a DIFFERENT fault (CS never reaching the
 * device, MISO sitting high) and must NOT be called blank. */
static void test_burst_blank(void)
{
    const uint8 blank[14]  = { 0u };
    const uint8 pulled[14] = {
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu
    };
    const uint8 live[14]   = {
        0xF4u, 0x00u,                                     /* temp raw 244     */
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,          /* gyro all zero    */
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u           /* accel X only     */
    };
    uint8 edge[14] = { 0u };
    uint8 i;

    CHECK_EQ(IMU_burstIsBlank(blank, 14u), TRUE);
    CHECK_EQ(IMU_burstIsBlank(pulled, 14u), FALSE);
    /* All six axis words zero is plausible; raw temp 244 makes it a sample. */
    CHECK_EQ(IMU_burstIsBlank(live, 14u), FALSE);

    /* Every single byte position must break the blank verdict - the driver's
     * only clue is one non-zero byte anywhere in the frame. */
    for (i = 0u; i < 14u; i++)
    {
        edge[i] = 0x01u;
        CHECK_EQ(IMU_burstIsBlank(edge, 14u), FALSE);
        edge[i] = 0u;
    }
}

int main(void)
{
    test_div_round();
    test_addr_bytes();
    test_ctrl_bytes();
    test_who_am_i();
    test_accel_conversions();
    test_gyro_conversions();
    test_temp_conversion();
    test_fs_index_mapping();
    test_le16();
    test_burst_decode();
    test_burst_blank();

    printf("test_imu: %d checks, %d failed\n", g_checks, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
