#ifndef IMU_H
#define IMU_H

#include "Ifx_Types.h"

/* LSM6DSV16BX six-axis IMU driver, CPU1 (doc 30-tc275/35-imu-driver.md,
 * wiring truth source doc/20-design/23-wiring.md section 10).
 *
 * Bus: QSPI1 master, SPI mode 3 (CPOL=1/CPHA=1), MSB first, auto-increment
 * bursts (IF_INC=1). First byte on the wire = bit7 R/W (1=read) + 7-bit
 * register address. Clock ladder 1/2/5/10 MHz, starts at 1 MHz (jumper-wire
 * discipline of doc 23 section 9.3 applies to these wires too).
 *
 * Ownership: CPU1 only (same core as the encoders and the battery ADC: the
 * real-time sensing core). IMU_task() runs inside the 1 kHz motor loop and
 * divides down to the ODR rate; nothing here may be called from another core.
 *
 * Data out: the xcore XcoreImu block (XCORE_imuPublish), seq-bumped on every
 * sample. INT1 (P15.4) is wired through ERU OGU0 and only COUNTED in v1 -
 * the counter is the bench evidence for the doc 23 section 10.2 first-check
 * step 3 ("INT1 pulse rate must match ODR"); reading is time-driven, not
 * DRDY-driven, until the V2.0 fusion work needs edge timestamps.
 *
 * Register facts below are cross-checked against the official ST driver
 * (stm32duino LSM6DSV16X lsm6dsv16x_reg.h/.c, ID 0x70) - the DSV16X map
 * differs from the older LSM6DSL: ODR lives in the LOW nibble of CTRL1/2,
 * gyro FS is the 4-bit CTRL6 field (4000 dps = 0xC) and accel FS the 2-bit
 * CTRL8 field. */

/* ---- register map (subset used by this driver) ---- */
#define IMU_REG_INT1_CTRL       0x0Du   /* drdy_xl bit0 / drdy_g bit1        */
#define IMU_REG_WHO_AM_I        0x0Fu
#define IMU_REG_CTRL1           0x10u   /* XL: odr[3:0] low, op_mode[6:4]    */
#define IMU_REG_CTRL2           0x11u   /* GY: odr[3:0] low, op_mode[6:4]    */
#define IMU_REG_CTRL3           0x12u   /* sw_reset bit0, if_inc bit2, bdu bit6 */
#define IMU_REG_CTRL6           0x15u   /* GY: fs_g[3:0]                     */
#define IMU_REG_CTRL8           0x17u   /* XL: fs_xl[1:0]                    */
#define IMU_REG_OUT_TEMP_L      0x20u   /* burst start: temp 2 + gyro 6 +
                                         * accel 6 = 14 bytes, contiguous    */
#define IMU_REG_OUTX_L_A        0x28u

#define IMU_WHO_AM_I_VAL        0x70u   /* LSM6DSV16X family ID              */

/* CTRL3_C bits */
#define IMU_CTRL3_SW_RESET      0x01u
#define IMU_CTRL3_IF_INC        0x04u   /* address auto-increment, required  */
#define IMU_CTRL3_BDU           0x40u   /* block data update: atomic L/H pair */

/* STATUS_REG bits (bench use only) */
#define IMU_STATUS_XLDA         0x01u
#define IMU_STATUS_GDA          0x02u

/* INT1_CTRL bits: one pulse per ODR period when both axes share the ODR */
#define IMU_INT1_DRDY_XL        0x01u
#define IMU_INT1_DRDY_G         0x02u

/* ---- output data rate codes (CTRL1/CTRL2 low nibble, high-perf mode) ---- */
#define IMU_ODR_OFF             0x0u
#define IMU_ODR_15HZ            0x3u
#define IMU_ODR_30HZ            0x4u
#define IMU_ODR_60HZ            0x5u
#define IMU_ODR_120HZ           0x6u
#define IMU_ODR_240HZ           0x7u
#define IMU_ODR_480HZ           0x8u
#define IMU_ODR_960HZ           0x9u

/* ---- full-scale codes ---- */
#define IMU_XL_FS_2G            0x0u
#define IMU_XL_FS_4G            0x1u
#define IMU_XL_FS_8G            0x2u
#define IMU_XL_FS_16G           0x3u

#define IMU_GY_FS_125DPS        0x0u
#define IMU_GY_FS_250DPS        0x1u
#define IMU_GY_FS_500DPS        0x2u
#define IMU_GY_FS_1000DPS       0x3u
#define IMU_GY_FS_2000DPS       0x4u
#define IMU_GY_FS_4000DPS       0xCu

/* ---- production defaults (doc 35 section 4): ±4 g / ±500 dps, 240 Hz.
 * ±4 g survives bumps a ±2 g range would clip; ±500 dps covers this
 * drivetrain's worst yaw rate with headroom; 240 Hz reads cleanly from the
 * 1 kHz loop every 4th tick. */
#define IMU_CFG_ODR             IMU_ODR_240HZ
#define IMU_CFG_XL_FS           IMU_XL_FS_4G
#define IMU_CFG_GY_FS           IMU_GY_FS_500DPS

/* ---- sensitivity, fixed point (official driver constants, exact):
 * accel mg/LSB = {0.061, 0.122, 0.244, 0.488}  -> num/1000
 * gyro  mdps/LSB = {35/8, 35/4, 35/2, 35/1, 70/1, 140/1} (125..4000 dps) */
#define IMU_ACC_MG_NUM_2G       61u
#define IMU_ACC_MG_NUM_4G       122u
#define IMU_ACC_MG_NUM_8G       244u
#define IMU_ACC_MG_NUM_16G      488u

#define IMU_GY_MDPS_NUM_125     35u
#define IMU_GY_MDPS_DEN_125     8u
#define IMU_GY_MDPS_NUM_250     35u
#define IMU_GY_MDPS_DEN_250     4u
#define IMU_GY_MDPS_NUM_500     35u
#define IMU_GY_MDPS_DEN_500     2u
#define IMU_GY_MDPS_NUM_1000    35u
#define IMU_GY_MDPS_DEN_1000    1u
#define IMU_GY_MDPS_NUM_2000    70u
#define IMU_GY_MDPS_NUM_4000    140u

/* temp: degC = raw/256 + 25 -> centiC = 2500 + raw*100/256 */
#define IMU_TEMP_OFF_CENTIC     2500
#define IMU_TEMP_NUM            100
#define IMU_TEMP_DEN            256

/* ---- SPI wire helpers (pure, host-tested) ---- */

/* First transaction byte: bit7 set = read, bits 6..0 = register address */
static inline uint8 IMU_readAddrByte(uint8 reg)
{
    return (uint8)(0x80u | (reg & 0x7Fu));
}

static inline uint8 IMU_writeAddrByte(uint8 reg)
{
    return (uint8)(reg & 0x7Fu);
}

/* CTRL1/CTRL2: ODR in the low nibble, op-mode 0 = high performance */
static inline uint8 IMU_ctrlOdrByte(uint8 odr)
{
    return (uint8)(odr & 0x0Fu);
}

/* CTRL3: always IF_INC (the burst reads depend on it), BDU on, no reset */
static inline uint8 IMU_ctrl3Byte(void)
{
    return (uint8)(IMU_CTRL3_IF_INC | IMU_CTRL3_BDU);
}

/* Symmetric integer division with rounding half away from zero - the unit
 * conversions below round, they never bias one direction (a raw of -1 on a
 * symmetric scale must not convert to 0). den > 0. */
static inline sint32 IMU_divRound(sint32 num, sint32 den)
{
    if (num >= 0)
    {
        return (num + (den / 2)) / den;
    }
    return -((-num + (den / 2)) / den);
}

/* raw -> milli-g for the configured accel FS code (2..16 g, others clamp to
 * the default ±4 g) */
static inline uint8 IMU_xlFsIndex(uint8 code)
{
    return (code <= IMU_XL_FS_16G) ? code : (uint8)IMU_XL_FS_4G;
}

static inline sint32 IMU_rawToMilliG(sint16 raw, uint8 xlFsCode)
{
    static const uint16 num[4] = {
        IMU_ACC_MG_NUM_2G, IMU_ACC_MG_NUM_4G, IMU_ACC_MG_NUM_8G, IMU_ACC_MG_NUM_16G
    };

    return IMU_divRound((sint32)raw * (sint32)num[IMU_xlFsIndex(xlFsCode)], 1000);
}

/* gyro FS code -> table row: 0xC (4000 dps) is out of numeric order */
static inline uint8 IMU_gyFsIndex(uint8 code)
{
    if (code == IMU_GY_FS_4000DPS)
    {
        return 5u;
    }
    return (code <= IMU_GY_FS_2000DPS) ? code : (uint8)IMU_GY_FS_500DPS;
}

/* raw -> milli-deg/s for the configured gyro FS code */
static inline sint32 IMU_rawToMilliDps(sint16 raw, uint8 gyFsCode)
{
    static const uint16 num[6] = {
        IMU_GY_MDPS_NUM_125, IMU_GY_MDPS_NUM_250, IMU_GY_MDPS_NUM_500,
        IMU_GY_MDPS_NUM_1000, IMU_GY_MDPS_NUM_2000, IMU_GY_MDPS_NUM_4000
    };
    static const uint8 den[6] = {
        IMU_GY_MDPS_DEN_125, IMU_GY_MDPS_DEN_250, IMU_GY_MDPS_DEN_500,
        IMU_GY_MDPS_DEN_1000, 1u, 1u
    };

    return IMU_divRound((sint32)raw * (sint32)num[IMU_gyFsIndex(gyFsCode)],
                        (sint32)den[IMU_gyFsIndex(gyFsCode)]);
}

/* raw -> die temperature in 0.01 degC (25.00 degC at raw 0) */
static inline sint32 IMU_rawToCentiC(sint16 raw)
{
    return IMU_TEMP_OFF_CENTIC
         + IMU_divRound((sint32)raw * IMU_TEMP_NUM, IMU_TEMP_DEN);
}

/* ---- clock ladder (doc 23 section 10.2: start 1 MHz, bench-verify each
 * rung, datasheet ceiling 10 MHz) ---- */
typedef enum
{
    IMU_CLK_1M = 0,
    IMU_CLK_2M,
    IMU_CLK_5M,
    IMU_CLK_10M,
    IMU_CLK_COUNT
} ImuClockTier;

typedef enum
{
    IMU_OK = 0,
    IMU_ERR_PARAM,        /* bad tier / register address / length        */
    IMU_ERR_NO_DEV,       /* WHO_AM_I mismatch, sensor absent or dead    */
    IMU_ERR_BUSY,         /* QSPI still holds the previous transaction   */
    IMU_ERR_HW,           /* QSPI latched an error flag                  */
    IMU_ERR_TIMEOUT       /* transaction never completed, module re-armed */
} ImuStatus;

/* ---- driver API, CPU1 only ---- */
void    IMU_init(void);            /* QSPI1 + ERU + probe + config, once    */
void    IMU_task(void);            /* 1 kHz call: ODR-rate read + publish   */
boolean IMU_isAlive(void);         /* WHO_AM_I ok and last read succeeded   */
uint8   IMU_whoAmI(void);          /* last probe result                     */
uint32  IMU_drdyCount(void);       /* INT1 rising edges since boot (ERU)    */
uint32  IMU_errCount(void);        /* failed SPI transactions since boot    */

/* Raw register access, CPU1 only (bench/diagnostic use). readRegs relies on
 * IF_INC=1 for bursts; len <= 14 (1 addr byte + 14 data = the frame buffer). */
ImuStatus IMU_readRegs(uint8 reg, uint8 *dst, uint16 len);
ImuStatus IMU_writeReg(uint8 reg, uint8 val);

ImuClockTier IMU_getClockTier(void);
ImuStatus    IMU_setClockTier(ImuClockTier tier);   /* refuse while busy   */
uint32       IMU_actualClockHz(void);   /* post-divider value, bench evidence  */

#endif /* IMU_H */
