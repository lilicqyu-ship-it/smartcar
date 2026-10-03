#include "bsp/imu.h"

#include "Qspi/SpiMaster/IfxQspi_SpiMaster.h"
#include "Qspi/Std/IfxQspi.h"
#include "Port/Std/IfxPort.h"
#include "Scu/Std/IfxScuEru.h"
#include "IfxSrc.h"
#include "bsp/stime.h"
#include "mw/xcore/xcore.h"

#include <string.h>

/* LSM6DSV16BX six-axis IMU, CPU1 owner (doc 30-tc275/35-imu-driver.md).
 *
 * Wiring truth source: doc/20-design/23-wiring.md section 10 - QSPI1
 * (SCLK=P11.6, MTSR=P11.9, MRST=P11.3, CS=SLSO3 P11.10), INT1=P15.4 through
 * ERU input channel 0 -> OGU0, VCC from the kit VEXT rail. SPI mode 3, MSB
 * first, 1 MHz start (jumper wires, same electrical situation as the QSPI3
 * link: no series termination, so the clock ladder is the only countermeasure
 * and the default must not exceed the first rung).
 *
 * Reading is time-driven from the 1 kHz motor loop (every 5th tick = 200 Hz,
 * just under the 240 Hz ODR so BDU hands over a fresh, atomic sample set).
 * INT1 is wired and counted but not used for scheduling in v1: the DRDY
 * counter is the bench evidence that INT1 pulses at the ODR (doc 23 section
 * 10.2 first-check step 3). Edge-driven reads + timestamps are the V2.0
 * fusion work's problem.
 *
 * Vector table 0 on all cores; the SRC TOS bit picks CPU1 (SDD C1). ISR
 * priorities are globally unique across the three cores (SDD C2 occupancy
 * table): 11/14/15 for QSPI1 TX/RX/ER, 24 for the ERU DRDY counter - below
 * the encoder block (16..23) so wheel edges are never held off by FIFO
 * service, and the (rare) DRDY edge can preempt anything on this core. */

#define IMU_VECTAB              0
#define IMU_TX_PRIO             11u
#define IMU_RX_PRIO             14u
#define IMU_ER_PRIO             15u
#define IMU_DRDY_PRIO           24u

/* fPER/tQspi base for the ladder: module clock is set for the top rung (the
 * 10 MHz datasheet ceiling) once, lower rungs go through the channel divider
 * - a tier switch never re-initialises the module. */
#define IMU_MODULE_CLK          10000000.0f

/* 1 header byte + the longest burst (temp 2 + gyro 6 + accel 6) */
#define IMU_MAX_DATA            14u
#define IMU_MAX_FRAME           (1u + IMU_MAX_DATA)

/* A 15-byte frame is ~120 us at 1 MHz; 5 ms is a hardware-fault detector */
#define IMU_XFER_TIMEOUT_MS     5u

/* task cadence: IMU_task runs at 1 kHz, the read every 5th tick */
#define IMU_TASK_DIV_MS         5u
/* dead-sensor recovery: re-probe rate while !alive, and how many consecutive
 * failed reads drop alive (5 ms apart -> 100 ms of bus failure) */
#define IMU_REPROBE_MS          1000u
#define IMU_ALIVE_FAIL_LIMIT    20u
/* bench telemetry line cadence (encoder-style IMU= row, 0.5 Hz) */
#define IMU_LOG_PERIOD_MS       2000u

/* INT1 = P15.4, push-pull driven by the module; pull-down defines the idle
 * level while the sensor is absent. If the IMU is ever configured open-drain
 * instead, this must flip to pullUp in the same change (doc 23 section 10.2
 * INT electrical note). */
#define IMU_INT1_PORT           (&MODULE_P15)
#define IMU_INT1_PIN            4u

/* CS = SLSO3 = P11.10. iLLD drives it as a plain GPIO around each exchange
 * (activate/deactivateSlso use IfxPort_setPinState), so bench code may use
 * the same primitives while the sensor is absent. */
#define IMU_CS_PORT             (&MODULE_P11)
#define IMU_CS_PIN              10u

/* Bench CS probe (dead state only): pull CS low for this many 1 kHz ticks
 * each second so the wire can be verified with a multimeter or LED instead
 * of a scope race against a 16 us pulse. Purely diagnostic, no protocol. */
#define IMU_CS_WIGGLE_TICKS     300u

static IfxQspi_SpiMaster         g_spi;
static IfxQspi_SpiMaster_Channel g_spiChannel;
static ImuClockTier              g_tier = IMU_CLK_1M;

static uint8 g_txBuf[IMU_MAX_FRAME];
static uint8 g_rxBuf[IMU_MAX_FRAME];

static volatile uint32 g_drdyCount;        /* ERU ISR writer, task reader  */

static boolean g_alive;
static uint8   g_whoAmI;
static uint32  g_errCount;                 /* failed transactions          */
static uint8   g_failRun;                  /* consecutive failed reads     */

static uint32 g_taskDiv;
static uint32 g_reprobeMs;                 /* next re-probe deadline       */
static uint32 g_logMs;                     /* next IMU= line deadline      */
static uint16 g_csWiggle;                  /* dead-state CS probe counter  */
static boolean g_csWiggleAnnounced;

static const uint32 g_clockHz[IMU_CLK_COUNT] = {
    1000000u, 2000000u, 5000000u, 10000000u
};

/* ---- ISRs ---- */

IFX_INTERRUPT(imuTxISR, IMU_VECTAB, IMU_TX_PRIO);

void imuTxISR(void)
{
    IfxQspi_SpiMaster_isrTransmit(&g_spi);
}

IFX_INTERRUPT(imuRxISR, IMU_VECTAB, IMU_RX_PRIO);

void imuRxISR(void)
{
    IfxQspi_SpiMaster_isrReceive(&g_spi);
}

IFX_INTERRUPT(imuErISR, IMU_VECTAB, IMU_ER_PRIO);

void imuErISR(void)
{
    IfxQspi_SpiMaster_isrError(&g_spi);
    g_errCount++;
}

IFX_INTERRUPT(imuDrdyISR, IMU_VECTAB, IMU_DRDY_PRIO);

/* INT1 data-ready edge: counted only (bench ODR evidence + future fusion
 * timestamp hook). Never start a SPI transaction here - the 1 kHz task owns
 * the bus.
 *
 * The latched event flag (SCU_EIFR.INTF0) is what feeds TRx0 -> OGU0 -> the
 * service request, so the request line stays asserted until it is cleared:
 * without the FMR write here the hardware acknowledge would immediately
 * re-pend the interrupt at the 240 Hz ODR and starve this core's 1 kHz loop. */
void imuDrdyISR(void)
{
    IfxScuEru_clearEventFlag(IfxScuEru_InputChannel_0);
    g_drdyCount++;
}

/* ---- QSPI1 (same driver as com/spi_hal_pins.c, different module) ---- */

static void imu_configureChannel(void)
{
    IfxQspi_SpiMaster_ChannelConfig chConfig;

    IfxQspi_SpiMaster_initChannelConfig(&chConfig, &g_spi);

    chConfig.ch.baudrate = (float32)g_clockHz[g_tier];

    /* P11.10 is SLSO3, the chip select the wiring doc locks for this bus.
     * Speed1 keeps the CS edge slow on the jumper wire - same reasoning as
     * the QSPI3 channel (a fast CS edge plus a thin ground return showed up
     * as phantom bits there). Shield2Go slot 2 shares this pad and must stay
     * empty while the IMU is in use (doc 23 section 10.1). */
    chConfig.sls.output.pin    = &IfxQspi1_SLSO3_P11_10_OUT;
    chConfig.sls.output.mode   = IfxPort_OutputMode_pushPull;
    chConfig.sls.output.driver = IfxPort_PadDriver_cmosAutomotiveSpeed1;

    /* CS low for the complete frame: with channelBasedCs disabled iLLD sets
     * LAST=1 only on the last word and releases CS once rx.remaining hits 0.
     * The ST device requires the whole addr+data frame inside one CS. */
    chConfig.channelBasedCs = IfxQspi_SpiMaster_ChannelBasedCs_disabled;
    chConfig.mode           = IfxQspi_SpiMaster_Mode_short;

    /* SPI mode 3 (CPOL=1/CPHA=1): ECON.CPOL=1 = idleHigh, and CPH=1 comes
     * from shiftTransmitDataOnLeadingEdge (IfxQspi.c maps leading -> CPH=1).
     * The names read backwards: CPH is the master's CAPTURE edge, so "shift
     * on leading" = sample on trailing = ST CPHA=1. Same trap as the QSPI3
     * channel comment, opposite direction (that slave was mode 0). */
    chConfig.ch.mode.clockPolarity = IfxQspi_ClockPolarity_idleHigh;
    chConfig.ch.mode.shiftClock    = IfxQspi_ShiftClock_shiftTransmitDataOnLeadingEdge;

    (void)IfxQspi_SpiMaster_initChannel(&g_spiChannel, &chConfig);
}

/* Module + INT1 pin + channel. Split out so a stalled transaction can be
 * re-armed without touching the config state. */
static void imu_spiSetup(ImuClockTier tier)
{
    IfxQspi_SpiMaster_Config config;

    IfxQspi_SpiMaster_initModuleConfig(&config, &MODULE_QSPI1);

    config.mode            = IfxQspi_Mode_master;
    config.maximumBaudrate = IMU_MODULE_CLK;
    config.txPriority      = IMU_TX_PRIO;
    config.rxPriority      = IMU_RX_PRIO;
    config.erPriority      = IMU_ER_PRIO;
    config.isrProvider     = IfxSrc_Tos_cpu1;

    /* iLLD defaults kept for the rest: interrupt per FIFO word, no DMA, no
     * buffering - one owner (CPU1), exchange() must report busy rather than
     * queue a second frame behind an unfinished one. */

    const IfxQspi_SpiMaster_Pins pins = {
        &IfxQspi1_SCLK_P11_6_OUT, IfxPort_OutputMode_pushPull,
        &IfxQspi1_MTSR_P11_9_OUT, IfxPort_OutputMode_pushPull,
        &IfxQspi1_MRSTB_P11_3_IN, IfxPort_InputMode_pullDown,
        IfxPort_PadDriver_cmosAutomotiveSpeed3
    };
    config.pins = &pins;

    IfxQspi_SpiMaster_initModule(&g_spi, &config);

    g_tier = tier;
    imu_configureChannel();
}

/* INT1 (P15.4) -> ERU input channel 0 (REQ0, RxSel a) -> OGU0 -> SRC_SCUERU0
 * -> CPU1. Rising edge only: INT1 is push-pull, high-active. */
static void imu_eruSetup(void)
{
    volatile Ifx_SRC_SRCR *src;

    IfxPort_setPinModeInput(IMU_INT1_PORT, IMU_INT1_PIN, IfxPort_InputMode_pullDown);

    IfxScuEru_selectExternalInput(IfxScuEru_InputChannel_0,
                                  IfxScuEru_ExternalInputSelection_0);
    IfxScuEru_enableRisingEdgeDetection(IfxScuEru_InputChannel_0);
    IfxScuEru_disableFallingEdgeDetection(IfxScuEru_InputChannel_0);
    IfxScuEru_enableTriggerPulse(IfxScuEru_InputChannel_0);
    IfxScuEru_connectTrigger(IfxScuEru_InputChannel_0, IfxScuEru_InputNodePointer_0);

    IfxScuEru_clearOutputChannelConfiguration(IfxScuEru_OutputChannel_0);
    IfxScuEru_setInterruptGatingPattern(IfxScuEru_OutputChannel_0,
                                        IfxScuEru_InterruptGatingPattern_alwaysActive);
    IfxScuEru_clearEventFlag(IfxScuEru_InputChannel_0);

    src = &MODULE_SRC.SCU.SCU.ERU[0];
    IfxSrc_init(src, IfxSrc_Tos_cpu1, IMU_DRDY_PRIO);
    IfxSrc_enable(src);
}

/* ---- register layer ---- */

static ImuStatus imu_xfer(uint8 addrByte, const uint8 *tx, uint8 *rx, uint16 len)
{
    uint16 total = (uint16)(1u + len);
    uint16 i;
    uint32 errorsBefore = g_errCount;
    uint32 start;
    IfxQspi_Status qspiStatus;

    if (len > IMU_MAX_DATA)
    {
        return IMU_ERR_PARAM;
    }

    /* Full-duplex silicon, half-duplex protocol: reads clock out 0xFF filler
     * (ignored by the device), writes discard the MISO echo. */
    g_txBuf[0] = addrByte;
    for (i = 0u; i < len; i++)
    {
        g_txBuf[i + 1u] = (rx != NULL_PTR) ? 0xFFu : tx[i];
    }

    qspiStatus = IfxQspi_SpiMaster_exchange(&g_spiChannel, g_txBuf, g_rxBuf,
                                            (Ifx_SizeT)total);
    if (qspiStatus == IfxQspi_Status_busy)
    {
        return IMU_ERR_BUSY;
    }
    if (qspiStatus != IfxQspi_Status_ok)
    {
        g_errCount++;
        return IMU_ERR_HW;
    }

    start = STIME_nowMs();
    while (IfxQspi_SpiMaster_getStatus(&g_spiChannel) == IfxQspi_Status_busy)
    {
        if ((uint32)(STIME_nowMs() - start) >= IMU_XFER_TIMEOUT_MS)
        {
            g_errCount++;
            /* Only a re-arm frees the module lock (the ER path does it for
             * bus errors; a lost service request needs this one). */
            imu_spiSetup(g_tier);
            return IMU_ERR_TIMEOUT;
        }
    }

    if (g_errCount != errorsBefore)
    {
        /* The error ISR latched a bad word and already released CS/lock. */
        return IMU_ERR_HW;
    }

    if (rx != NULL_PTR)
    {
        for (i = 0u; i < len; i++)
        {
            rx[i] = g_rxBuf[i + 1u];
        }
    }

    return IMU_OK;
}

ImuStatus IMU_readRegs(uint8 reg, uint8 *dst, uint16 len)
{
    if ((dst == NULL_PTR) || (len == 0u))
    {
        return IMU_ERR_PARAM;
    }
    return imu_xfer(IMU_readAddrByte(reg), NULL_PTR, dst, len);
}

ImuStatus IMU_writeReg(uint8 reg, uint8 val)
{
    return imu_xfer(IMU_writeAddrByte(reg), &val, NULL_PTR, 1u);
}

/* ---- probe + configuration (also the re-probe path) ---- */

/* IMU_OK only when WHO_AM_I read clean AND matched. Any other code is the
 * bench evidence for why it did not (doc 35 section 7.1). */
static ImuStatus imu_probe(void)
{
    uint8     id = 0u;
    ImuStatus st;

    st = IMU_readRegs(IMU_REG_WHO_AM_I, &id, 1u);
    if (st != IMU_OK)
    {
        return st;
    }
    g_whoAmI = id;
    return (id == IMU_WHO_AM_I_VAL) ? IMU_OK : IMU_ERR_NO_DEV;
}

/* The raw byte is what separates the causes the absent line cannot: 0x00 =
 * MISO never driven (wire/pad), 0x38 or 0xE2 = the frame arrived bit-shifted
 * (clock phase; 0x71 >> 1 and 0x71 << 1), 0xFF = CS never reached the device,
 * anything else = a part that is not the expected one. st = 0 here means the
 * probe matched and a later configuration write is what failed. */
static void imu_logProbeFailure(ImuStatus st)
{
    sint32 vals[4];

    vals[0] = (sint32)st;
    vals[1] = (sint32)g_whoAmI;
    vals[2] = (sint32)g_errCount;
    vals[3] = (sint32)IMU_actualClockHz();
    XCORE_logi("IMUERR st/id/err/clk=", vals, 4u);
}

static boolean imu_configure(void)
{
    uint32 t0;
    uint8  val;

    /* Reset the register map first (a noisy power-up can leave CTRL regs in
     * any state), then wait for the bit to self-clear before configuring. */
    if (IMU_writeReg(IMU_REG_CTRL3, IMU_CTRL3_SW_RESET) != IMU_OK)
    {
        return FALSE;
    }

    t0 = STIME_nowMs();
    do
    {
        if (IMU_readRegs(IMU_REG_CTRL3, &val, 1u) != IMU_OK)
        {
            return FALSE;
        }
    } while (((val & IMU_CTRL3_SW_RESET) != 0u)
             && ((uint32)(STIME_nowMs() - t0) < IMU_XFER_TIMEOUT_MS));

    if ((val & IMU_CTRL3_SW_RESET) != 0u)
    {
        return FALSE;
    }

    /* Datasheet: registers need a settle interval after SW_RESET before the
     * next write. One-time cost at boot, a busy wait is fine here. */
    STIME_delayMs(5u);

    if (IMU_writeReg(IMU_REG_CTRL3, IMU_ctrl3Byte()) != IMU_OK)        /* IF_INC|BDU */
    {
        return FALSE;
    }
    /* Data-ready as a pulse, not latched: with the default the line stays
     * high until the data registers are read and the ERU DRDY counter
     * measured our 200 Hz read cadence instead of the 240 Hz ODR
     * (bench evidence, doc 35 section 7 step 3). */
    if (IMU_writeReg(IMU_REG_CTRL4, 0x02u) != IMU_OK)
    {
        return FALSE;
    }
    if (IMU_writeReg(IMU_REG_CTRL1, IMU_ctrlOdrByte(IMU_CFG_ODR)) != IMU_OK)
    {
        return FALSE;
    }
    if (IMU_writeReg(IMU_REG_CTRL2, IMU_ctrlOdrByte(IMU_CFG_ODR)) != IMU_OK)
    {
        return FALSE;
    }
    if (IMU_writeReg(IMU_REG_CTRL6, (uint8)(IMU_CFG_GY_FS & 0x0Fu)) != IMU_OK)
    {
        return FALSE;
    }
    if (IMU_writeReg(IMU_REG_CTRL8, (uint8)(IMU_CFG_XL_FS & 0x03u)) != IMU_OK)
    {
        return FALSE;
    }
    /* INT1 pulses on data ready (XL and GY share the ODR -> one pulse per
     * period): the ERU counter's expected rate is the ODR. */
    if (IMU_writeReg(IMU_REG_INT1_CTRL,
                     (uint8)(IMU_INT1_DRDY_XL | IMU_INT1_DRDY_G)) != IMU_OK)
    {
        return FALSE;
    }

    return TRUE;
}

/* ---- publish ---- */

static void imu_publish(void)
{
    /* Burst layout, little-endian pairs (d[0] = the first byte read after the
     * 0x20 command byte): temp d[0:1] | gyro d[2:7] in X,Y,Z | accel d[8:13]
     * in the register order Z,Y,X, i.e. X = d[12:13], Y = d[10:11], Z = d[8:9]. */
    const uint8 *d = &g_rxBuf[1];
    XcoreImu imu;
    uint8 i;

    imu.alive    = g_alive;
    imu.whoAmI   = g_whoAmI;
    for (i = 0u; i < 3u; i++)
    {
        imu.accMilliG[i]    = (sint16)IMU_rawToMilliG(IMU_burstAccRaw(d, i),
                                                     IMU_CFG_XL_FS);
        imu.gyroMilliDps[i] = IMU_rawToMilliDps(IMU_burstGyroRaw(d, i),
                                                IMU_CFG_GY_FS);
    }
    imu.tempCentiC = (sint16)IMU_rawToCentiC(IMU_burstTempRaw(d));
    imu.drdyCount = g_drdyCount;
    imu.errCount  = g_errCount;

    XCORE_imuPublish(&imu);
}

/* One 14-byte burst: temp + gyro + accel, auto-increment from 0x20. Returns
 * TRUE on a clean transaction carrying data; failure counting lives in
 * imu_xfer and in IMU_task's fail-run.
 *
 * An all-zero payload counts as a failure. A module that brownouts and resets
 * itself (CTRL1/2 back to ODR off) stops clocking INT1 and answers reads with
 * 0x00 while every QSPI transaction stays clean - so status alone reports it
 * as healthy, alive never drops, and the fusion layer keeps being fed a flat
 * zero stream. Treating it as a failed read hands it to the existing recovery:
 * 20 x 5 ms -> alive FALSE -> 1 Hz re-probe reconfigures the part, and if it
 * really is unpowered the serial shows IMUERR instead of a lying IMU row. */
static boolean imu_readData(void)
{
    if (IMU_readRegs(IMU_REG_OUT_TEMP_L, &g_rxBuf[1], IMU_MAX_DATA) != IMU_OK)
    {
        return FALSE;
    }
    return !IMU_burstIsBlank(&g_rxBuf[1], (uint8)IMU_MAX_DATA);
}

void IMU_init(void)
{
    ImuStatus st;

    memset(g_txBuf, 0, sizeof(g_txBuf));
    memset(g_rxBuf, 0, sizeof(g_rxBuf));
    g_alive      = FALSE;
    g_whoAmI     = 0u;
    g_errCount   = 0u;
    g_failRun    = 0u;
    g_taskDiv    = 0u;
    g_drdyCount  = 0u;
    g_csWiggle   = 0u;
    g_csWiggleAnnounced = FALSE;

    imu_spiSetup(IMU_CLK_1M);
    imu_eruSetup();

    g_reprobeMs = STIME_nowMs() + IMU_REPROBE_MS;
    g_logMs     = STIME_nowMs() + IMU_LOG_PERIOD_MS;

    st = imu_probe();
    if ((st == IMU_OK) && imu_configure())
    {
        g_alive = TRUE;
        g_csWiggleAnnounced = FALSE;
        (void)imu_readData();
        imu_publish();
        XCORE_logln("IMU ready (WHOAMI=0x71, ODR 240Hz, +-4g/+-500dps, 1MHz)");
    }
    else
    {
        /* The 1 kHz task retries once a second - wiring the sensor in
         * without a reboot is the normal bench flow. */
        XCORE_logln("IMU absent (WHOAMI mismatch or bus dead), retrying 1Hz");
        imu_logProbeFailure(st);
    }
}

void IMU_task(void)
{
    ImuStatus st;

    g_taskDiv++;

    if (!g_alive)
    {
        /* Bench aid while dead: wiggle CS (300 ms low per second) so the
         * wire can be checked without a scope. Runs on the wait ticks only;
         * the probe tick below lets iLLD own the pin again. */
        if (!g_csWiggleAnnounced)
        {
            g_csWiggleAnnounced = TRUE;
            XCORE_logln("IMU CS wiggle 300ms/s until sensor found");
        }
        if (g_csWiggle < IMU_CS_WIGGLE_TICKS)
        {
            IfxPort_setPinState(IMU_CS_PORT, IMU_CS_PIN, IfxPort_State_low);
        }
        else
        {
            IfxPort_setPinState(IMU_CS_PORT, IMU_CS_PIN, IfxPort_State_high);
        }
        g_csWiggle = (uint16)((g_csWiggle + 1u) % 1000u);

        /* Dead/absent sensor: stay cheap, retry the probe once a second. */
        if ((uint32)(STIME_nowMs() - g_reprobeMs) < IMU_REPROBE_MS)
        {
            return;
        }
        g_csWiggle = 0u;                     /* probe tick: iLLD owns CS    */
        g_reprobeMs = STIME_nowMs();
        st = imu_probe();
        if ((st == IMU_OK) && imu_configure())
        {
            g_failRun = 0u;
            g_alive   = TRUE;
            g_csWiggleAnnounced = FALSE;
            IfxPort_setPinState(IMU_CS_PORT, IMU_CS_PIN, IfxPort_State_high);
            XCORE_logln("IMU found (WHOAMI=0x71), configured");
            return;
        }
        /* Report the cause, but at the same 0.5 Hz discipline as the IMU= row:
         * a dead sensor must not consume the log ring at 1 Hz. */
        if ((uint32)(STIME_nowMs() - g_logMs) >= IMU_LOG_PERIOD_MS)
        {
            g_logMs = STIME_nowMs();
            imu_logProbeFailure(st);
        }
        return;
    }

    if ((g_taskDiv % IMU_TASK_DIV_MS) != 0u)
    {
        return;
    }

    if (imu_readData())
    {
        g_failRun = 0u;
        imu_publish();
    }
    else
    {
        g_failRun++;
        if (g_failRun >= IMU_ALIVE_FAIL_LIMIT)
        {
            g_alive     = FALSE;
            g_reprobeMs = STIME_nowMs();
            XCORE_logln("IMU bus lost (read failures), probing 1Hz");
        }
    }

    if ((uint32)(STIME_nowMs() - g_logMs) >= IMU_LOG_PERIOD_MS)
    {
        XcoreImu snap;
        sint32 vals[10];

        g_logMs = STIME_nowMs();
        XCORE_imuRead(&snap);
        vals[0] = (sint32)snap.whoAmI;
        vals[1] = snap.accMilliG[0];
        vals[2] = snap.accMilliG[1];
        vals[3] = snap.accMilliG[2];
        vals[4] = snap.gyroMilliDps[0];
        vals[5] = snap.gyroMilliDps[1];
        vals[6] = snap.gyroMilliDps[2];
        vals[7] = snap.tempCentiC;
        vals[8] = (sint32)snap.drdyCount;
        vals[9] = (sint32)snap.errCount;
        XCORE_logi("IMU", vals, 10u);
    }
}

boolean IMU_isAlive(void)
{
    return g_alive;
}

uint8 IMU_whoAmI(void)
{
    return g_whoAmI;
}

uint32 IMU_drdyCount(void)
{
    return g_drdyCount;
}

uint32 IMU_errCount(void)
{
    return g_errCount;
}

ImuClockTier IMU_getClockTier(void)
{
    return g_tier;
}

ImuStatus IMU_setClockTier(ImuClockTier tier)
{
    if (tier >= IMU_CLK_COUNT)
    {
        return IMU_ERR_PARAM;
    }
    if (IfxQspi_SpiMaster_getStatus(&g_spiChannel) == IfxQspi_Status_busy)
    {
        return IMU_ERR_BUSY;
    }

    g_tier = tier;
    (void)IfxQspi_SpiMaster_setChannelBaudrate(&g_spiChannel,
                                               (float32)g_clockHz[tier]);
    return IMU_OK;
}

uint32 IMU_actualClockHz(void)
{
    /* iLLD stores the post-divider value here, not the requested one. */
    return (uint32)g_spiChannel.baudrate;
}
