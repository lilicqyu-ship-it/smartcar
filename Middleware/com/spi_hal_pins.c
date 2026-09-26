/*
 * spi_hal_pins.c - TC275 QSPI3 master half of the production LINK
 *
 * See spi_hal_pins.h for the pin map, the preamble simulation and the command
 * byte table. Owner core: CPU2 (bare metal). No FreeRTOS API is used here.
 */
#include "spi_hal_pins.h"

#include "Qspi/SpiMaster/IfxQspi_SpiMaster.h"
#include "Qspi/Std/IfxQspi.h"
#include "Port/Std/IfxPort.h"
#include "stime.h"

#include <string.h>

/* Vector table 0 on all three cores; the SRC TOS bit picks CPU2. Declaring any
 * other table number makes the Tasking lsl drop the entry silently (SDD C1). */
#define SPIHAL_VECTAB          0
/* Free slots next to ASCLIN1's 5/7/13 on CPU2 (SDD C2 occupancy table). */
#define SPIHAL_TX_PRIO         6
#define SPIHAL_RX_PRIO         9
#define SPIHAL_ER_PRIO         10

/* A max-length segment is 263 bytes; at the slowest ladder rung (1 MHz) that is
 * 2.1 ms on the wire, so 10 ms is a hardware-fault detector, not a pace. */
#define SPIHAL_XFER_TIMEOUT_MS 10u

/* fPER/tQspi base for the ladder: the module clock is set for the top rung once
 * and every rung below is reached through the channel divider, so a tier switch
 * never re-initialises the module and never interrupts the link. */
#define SPIHAL_MODULE_CLK      20000000.0f

/* P23.0: slave IRQ, open drain with an external pull-up -> plain input. */
#define SPIHAL_IRQ_PORT        (&MODULE_P23)
#define SPIHAL_IRQ_PIN         0u

static IfxQspi_SpiMaster          g_spi;
static IfxQspi_SpiMaster_Channel  g_spiChannel;
static SpiHal_ClockTier           g_tier = SPIHAL_CLK_1M;

static uint8        g_txBuf[SPIHAL_MAX_XFER];
static uint8        g_rxBuf[SPIHAL_MAX_XFER];
static SpiHal_Stats g_stats;

static const uint32 g_clockHz[SPIHAL_CLK_COUNT] = {
    1000000u, 2000000u, 5000000u, 10000000u, 20000000u
};

IFX_INTERRUPT(spiTxISR, SPIHAL_VECTAB, SPIHAL_TX_PRIO);

void spiTxISR(void)
{
    IfxQspi_SpiMaster_isrTransmit(&g_spi);
}

IFX_INTERRUPT(spiRxISR, SPIHAL_VECTAB, SPIHAL_RX_PRIO);

void spiRxISR(void)
{
    IfxQspi_SpiMaster_isrReceive(&g_spi);
}

IFX_INTERRUPT(spiErISR, SPIHAL_VECTAB, SPIHAL_ER_PRIO);

void spiErISR(void)
{
    IfxQspi_SpiMaster_isrError(&g_spi);
    g_stats.hwErrors++;
}

uint32 SPIHAL_clockHz(SpiHal_ClockTier tier)
{
    if (tier >= SPIHAL_CLK_COUNT)
    {
        return g_clockHz[SPIHAL_CLK_1M];
    }
    return g_clockHz[tier];
}

static void spiHal_configureChannel(void)
{
    IfxQspi_SpiMaster_ChannelConfig chConfig;

    IfxQspi_SpiMaster_initChannelConfig(&chConfig, &g_spi);

    chConfig.ch.baudrate = (float32)SPIHAL_clockHz(g_tier);

    /* P23.4 is SLSO5, the only chip select that is not taken by another
     * function on this kit (SDD C7). */
    chConfig.sls.output.pin    = &IfxQspi3_SLSO5_P23_4_OUT;
    chConfig.sls.output.mode   = IfxPort_OutputMode_pushPull;
    chConfig.sls.output.driver = IfxPort_PadDriver_cmosAutomotiveSpeed4;

    /* CS low for the complete exchange: with channelBasedCs disabled iLLD
     * writes a "begin stream" BACON before the first word and sets LAST=1 only
     * on the last one, and deactivateSlso() runs only once rx.remaining is 0.
     * That is what keeps CMD+ADDR+DUMMY+data inside one chip select, which is
     * the behaviour gate G1 exists to confirm. */
    chConfig.channelBasedCs = IfxQspi_SpiMaster_ChannelBasedCs_disabled;
    chConfig.mode           = IfxQspi_SpiMaster_Mode_short;

    /* iLLD defaults are already SPI mode 0 (clock idle low, sample on the
     * trailing edge), 8-bit words, MSB first, CS active low - what Espressif's
     * seg_master runs with (dev_cfg->mode = 0). Left untouched on purpose. */
    (void)IfxQspi_SpiMaster_initChannel(&g_spiChannel, &chConfig);
}

/* Module + handshake pin + channel. Split out of SPIHAL_init so a stalled
 * transaction can be re-armed without wiping the error history. */
static void spiHal_setup(SpiHal_ClockTier tier)
{
    IfxQspi_SpiMaster_Config config;

    IfxQspi_SpiMaster_initModuleConfig(&config, &MODULE_QSPI3);

    config.mode            = IfxQspi_Mode_master;
    config.maximumBaudrate = SPIHAL_MODULE_CLK;
    config.txPriority      = SPIHAL_TX_PRIO;
    config.rxPriority      = SPIHAL_RX_PRIO;
    config.erPriority      = SPIHAL_ER_PRIO;
    config.isrProvider     = IfxSrc_Tos_cpu2;

    /* The remaining fields keep their iLLD defaults, which is what this driver
     * wants: interrupt per FIFO word, useDma = FALSE, and bufferSize = 0 with
     * buffer = NULL_PTR. Buffering is deliberately off - with a buffer exchange()
     * accepts a second request while one is running, which would hide the busy
     * answer the pump needs (this module has exactly one owner: CPU2). */

    const IfxQspi_SpiMaster_Pins pins = {
        &IfxQspi3_SCLK_P33_11_OUT, IfxPort_OutputMode_pushPull,
        &IfxQspi3_MTSR_P33_12_OUT, IfxPort_OutputMode_pushPull,
        &IfxQspi3_MRSTD_P33_13_IN, IfxPort_InputMode_pullUp,
        IfxPort_PadDriver_cmosAutomotiveSpeed4
    };
    config.pins = &pins;

    IfxQspi_SpiMaster_initModule(&g_spi, &config);

    /* P23.0 pull-up input: the slave drives it open drain, the board supplies
     * the pull-up, the pump samples the level. P23.x is not an IOM monitor
     * input on this package, so no edge interrupt exists for this pin. */
    IfxPort_setPinModeInput(SPIHAL_IRQ_PORT, SPIHAL_IRQ_PIN, IfxPort_InputMode_pullUp);

    g_tier = tier;
    spiHal_configureChannel();
}

void SPIHAL_init(SpiHal_ClockTier tier)
{
    memset(&g_stats, 0, sizeof(g_stats));

    if (tier >= SPIHAL_CLK_COUNT)
    {
        tier = SPIHAL_CLK_1M;
    }
    spiHal_setup(tier);
}

SpiHal_Status SPIHAL_setClock(SpiHal_ClockTier tier)
{
    if (tier >= SPIHAL_CLK_COUNT)
    {
        return SPIHAL_ERR_PARAM;
    }
    if (SPIHAL_isBusy())
    {
        return SPIHAL_ERR_BUSY;
    }

    g_tier = tier;
    (void)IfxQspi_SpiMaster_setChannelBaudrate(&g_spiChannel,
                                              (float32)SPIHAL_clockHz(tier));
    return SPIHAL_OK;
}

SpiHal_ClockTier SPIHAL_getClock(void)
{
    return g_tier;
}

uint32 SPIHAL_actualClockHz(void)
{
    /* iLLD stores the post-divider value here, not the requested one. */
    return (uint32)g_spiChannel.baudrate;
}

boolean SPIHAL_isBusy(void)
{
    return (boolean)(IfxQspi_SpiMaster_getStatus(&g_spiChannel) == IfxQspi_Status_busy);
}

boolean SPIHAL_irqAsserted(void)
{
    return (boolean)(IfxPort_getPinState(SPIHAL_IRQ_PORT, SPIHAL_IRQ_PIN) != 0u);
}

void SPIHAL_getStats(SpiHal_Stats *stats)
{
    if (stats != NULL_PTR)
    {
        *stats = g_stats;
    }
}

SpiHal_Status SPIHAL_transaction(uint8 cmd, uint8 addr,
                                const uint8 *txData, uint8 *rxData,
                                uint16 dataLen, boolean read)
{
    uint16 total;
    uint16 i;
    uint32 errorsBefore;
    uint32 start;
    IfxQspi_Status qspiStatus;

    if ((dataLen > SPIHAL_MAX_DATA) || ((dataLen & 3u) != 0u)
        || ((dataLen > 0u) && (read == FALSE) && (txData == NULL_PTR))
        || ((dataLen > 0u) && (read != FALSE) && (rxData == NULL_PTR)))
    {
        return SPIHAL_ERR_PARAM;
    }

    total = (uint16)(SPIHD_PREAMBLE_LEN + dataLen);

    /* The bus is full duplex in silicon but half duplex in this protocol: the
     * master clocks the same number of bytes out and in, so a read sends 0xFF
     * filler (visible on the G1 trace, and ignored by the slave) and a write
     * discards the echo. */
    g_txBuf[0] = cmd;
    g_txBuf[1] = addr;
    g_txBuf[2] = 0x00u;                            /* DUMMY phase */
    for (i = SPIHD_PREAMBLE_LEN; i < total; i++)
    {
        g_txBuf[i] = (read != FALSE) ? 0xFFu : txData[i - SPIHD_PREAMBLE_LEN];
    }

    errorsBefore = g_stats.hwErrors;

    /* With bufferSize = 0 exchange() refuses to start while the previous
     * transaction still holds the module lock (IfxQspi_Status_busy). */
    qspiStatus = IfxQspi_SpiMaster_exchange(&g_spiChannel, g_txBuf, g_rxBuf,
                                            (Ifx_SizeT)total);
    if (qspiStatus == IfxQspi_Status_busy)
    {
        g_stats.busyRejects++;
        return SPIHAL_ERR_BUSY;
    }
    if (qspiStatus != IfxQspi_Status_ok)
    {
        g_stats.hwErrors++;
        return SPIHAL_ERR_HW;
    }
    g_stats.transactions++;

    start = STIME_nowMs();
    while (SPIHAL_isBusy())
    {
        if ((uint32)(STIME_nowMs() - start) >= SPIHAL_XFER_TIMEOUT_MS)
        {
            g_stats.timeouts++;
            /* A QSPI error latches an ER interrupt and iLLD's isrError unlocks
             * the module, so reaching here means the TX/RX service request was
             * lost instead. Only a re-arm frees the lock; the tier is kept
             * because nothing on the wire changed. */
            spiHal_setup(g_tier);
            return SPIHAL_ERR_TIMEOUT;
        }
    }

    if (g_stats.hwErrors != errorsBefore)
    {
        /* The error ISR latched a bad word and iLLD already released CS and the
         * module lock. Nothing is silently repaired here: the pump counts it as
         * SPI_ERR and the frame layer rejects the segment on CRC. */
        return SPIHAL_ERR_HW;
    }

    if (read != FALSE)
    {
        /* The slave's first three MISO bytes answer the preamble and carry no
         * payload. */
        for (i = 0u; i < dataLen; i++)
        {
            rxData[i] = g_rxBuf[SPIHD_PREAMBLE_LEN + i];
        }
    }

    return SPIHAL_OK;
}
