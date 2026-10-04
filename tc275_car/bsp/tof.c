#include "bsp/tof.h"

#include "I2c/Std/IfxI2c.h"
#include "IfxCpu.h"
#include "Port/Std/IfxPort.h"
#include "mw/xcore/xcore.h"

/* ST's Ultra Lite Driver, shipped verbatim in Libraries/ST/vl53l5cx.
 * The path is spelled relative to this file on purpose: ADS quietly drops a
 * hand-written "project root" include entry (doc/30-tc275/36-tof-driver.md
 * section 3), so the only portable form is one the preprocessor resolves from
 * the including file's own directory. api.h pulls its own platform.h from that
 * same directory - the one file there this repository owns. */
#include "../Libraries/ST/vl53l5cx/vl53l5cx_api.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

/* VL53L5CX multizone ToF, CPU0 owner (doc 30-tc275/36-tof-driver.md).
 *
 * Wiring truth source: doc/20-design/23-wiring.md section 11 - I2C0
 * (SCL=P02.5=X304-7, SDA=P02.4=X304-8), INT=P10.7=hole 15, AVDD and
 * IOVDD both from the kit VEXT 3.3 V rail, 2.2 k bus pull-ups plus the 47 k
 * pull-ups the datasheet demands on INT and LPn. Nothing here can work without
 * those resistors: IfxI2c_initSclSdaPin puts both pads in open-drain mode and
 * this I2C module has no internal pull-up to fall back on.
 *
 * Division of labour, and it is deliberate. Every register semantic, the 84 KB
 * device firmware, the DCI settings protocol and the result-page parsing live
 * in the vendored ULD (Libraries/ST/vl53l5cx, BSD-3, never edited). This file
 * owns exactly three things: the 16-bit-index wire format, transaction bounds
 * that cannot hang, and the bring-up ladder. Nothing in here re-implements a
 * register table - that is how the IMU batch wrote down 0x70 for a part that
 * answers 0x71 (doc 23 section 10.2).
 *
 * The bus layer is built from I2c/Std primitives only and the I2c/I2c module
 * driver stays excluded from the build. Reason: it disables global interrupts
 * for the length of a burst (a FreeRTOS service task must not), its waits on
 * TX_END and on the FIFO having a request are unbounded, and it pushes one word
 * per handshake, which cannot express a 32 KB firmware write without a 130 k
 * iteration interrupt-off window. What it does teach, and what this file reuses
 * as the packet shapes, is: an address probe, then a packet whose first byte is
 * the address again (ADDRCFG.SOPE=0 leaves the master in restart state, so that
 * second packet goes out as a repeated start), and a read as MRPS with TPS=1.
 * Every TX/RX word acknowledges its FIFO request before waiting for TX_END.
 *
 * Task placement: vTofTask sits one priority BELOW the robot task, and that is
 * the whole reason a 2.2 s firmware download and a ~0.7 ms frame read are
 * tolerable on this core. The robot task is what feeds the CPU0 watchdog every
 * 10 ms; it preempts this task whenever it is ready, so no transaction here can
 * delay that feed - and no deadline in here assumes this task keeps the CPU. */

#define TOF_I2C                 (&MODULE_I2C0)
#define TOF_SCL_PORT            (&MODULE_P02)
#define TOF_SDA_PORT            (&MODULE_P02)
#define TOF_SCL_PIN             5u
#define TOF_SDA_PIN             4u
/* Temporary bench isolation: stop ranging and park both pads as weak pull-up inputs.
 * Set back to 0 after the disconnected-SDA electrical check. */
#define TOF_GPIO_ISOLATION      0

/* User-requested 1 MHz bench trial after the 100 kHz ranging pass. FM+ requires
 * SCL/SDA tR <= 120 ns, which jumper wires behind 2.2 k pull-ups are not known
 * to meet until a scope has measured them (doc 23 section 11.3). */
#define TOF_I2C_BAUDRATE        1000000.0f

/* Ranging setup. 4x4 first, because doc 23 section 11.4 step 6 runs the first
 * bench pass at 4x4 with a 5 ms integration time, and because wiring that has
 * never been powered wants the shortest frame and the smallest result buffer it
 * can get; TOF_CFG_8X8 in bsp/tof.h moves the resolution and the zone count
 * together. AUTONOMOUS is what makes an integration time mean anything (in
 * continuous mode the sensor always integrates to maximum - vl53l5cx_api.h
 * says so at :58-63). CLOSEST target order: the consumer this is headed for is
 * obstacle avoidance, which wants the nearest surface, not the brightest. */
#if (TOF_CFG_8X8 != 0)
#define TOF_CFG_RESOLUTION      VL53L5CX_RESOLUTION_8X8
#else
#define TOF_CFG_RESOLUTION      VL53L5CX_RESOLUTION_4X4
#endif
#define TOF_CFG_FREQ_HZ         15u
#define TOF_CFG_INTEGRATION_MS  5u
#define TOF_CFG_TARGET_ORDER    VL53L5CX_TARGET_ORDER_CLOSEST
#define TOF_CFG_RANGING_MODE    VL53L5CX_RANGING_MODE_AUTONOMOUS

/* INT = P10.7, the module's open-drain output with its external 47 k pull-up.
 * Polled as a level, never an edge interrupt: ERU input channel 0 already
 * belongs to the IMU's INT1 on P15.4 and the two pins share that one channel
 * (doc 23 section 11.2). */
#define TOF_INT_PORT            (&MODULE_P10)
#define TOF_INT_PIN             7u

/* Dead-module discipline, taken from the IMU driver because it earned it:
 * re-probe once a second, so plugging the module in without a reboot works, and
 * drop alive after this many consecutive failed ULD calls. */
#define TOF_REPROBE_MS          1000u
#define TOF_ALIVE_FAIL_LIMIT    20u
#define TOF_LOG_PERIOD_MS       10000u

typedef enum
{
    TOF_ST_DEAD = 0,    /* bus layer not up (only before TOF_init)          */
    TOF_ST_PROBE,       /* identity check, once a second until it answers    */
    TOF_ST_INIT,        /* 84 KB firmware download                           */
    TOF_ST_CONFIG,      /* power mode, resolution, mode, IT, order, rate     */
    TOF_ST_RANGING      /* data-ready poll + frame read                      */
} TofState;

/* The ULD's whole state: offset_data[488] + xtalk_data[776] +
 * temp_buffer[1024] + the small print, about 2.3 KB of bss. Static rather than
 * a task local because vl53l5cx_get_ranging_data has to keep the streamcount
 * between calls, and because 2.3 KB is eight times the stack this task gets. */
static VL53L5CX_Configuration g_dev;
static VL53L5CX_ResultsData   g_results;

/* One message's worth of staging: slave address byte, index pair, payload. */
static uint8     g_txBuf[TOF_TX_MAX];

static TofState  g_state;
static boolean   g_alive;
static uint8     g_cfgStep;       /* which ladder step failed (0 = probe)   */
static uint8     g_stStatus;      /* last non-zero status out of the ULD    */
static uint8     g_whoAmI[2];     /* device_id/revision_id as last read off
                                   * the wire, 0 until a bus answer         */
static TofStatus g_lastErr;       /* last bus-layer failure code            */
static uint32    g_errCount;      /* failed I2C messages since boot         */
static uint8     g_failRun;       /* consecutive failed ULD calls           */
static uint32    g_frameCount;
static uint32    g_frameMs;
static FusionTof g_snapshot;
static uint32    g_initMs;        /* measured vl53l5cx_init() cost          */
static float32   g_actualHz;      /* post-divider bus clock, bench evidence */
static uint32    g_reprobeMs;
static uint32    g_logMs;         /* TOF= row deadline                      */
static uint32    g_errLogMs;      /* TOFERR= line deadline                  */

/* ---- timebase ---- */

static uint32 tof_nowMs(void)
{
    /* FreeRTOS tick at 1 kHz (configTICK_RATE_HZ) -> milliseconds. Only valid
     * inside a task: the tick does not advance before vTaskStartScheduler(),
     * which is precisely why TOF_init() is called from vTofTask and not from
     * core0_main(). */
    return (uint32)xTaskGetTickCount() * (uint32)portTICK_PERIOD_MS;
}

static boolean tof_expired(uint32 startMs)
{
    return ((uint32)(tof_nowMs() - startMs) >= TOF_XFER_TIMEOUT_MS) ? TRUE : FALSE;
}

/* ---- bus layer, I2c/Std primitives only ---- */

/* Preserve the first fault of each transaction before STOP/reset clears it.
 * phase/rw/reg/len/PIRQSS/ERRIRQSS/RIS/BS/FFS/SCL/SDA/TPS/address,
 * printed only at TOFERR cadence. */
static uint32 g_busDiag[13];
static uint32 g_busContext[6];
static boolean g_busCaptured;
static boolean g_busDiagValid;

static void tof_busBegin(boolean read, uint16 reg, uint16 len)
{
    g_busContext[0] = 1u; /* bus-free check */
    g_busContext[1] = (read != FALSE) ? 1u : 0u;
    g_busContext[2] = (uint32)reg;
    g_busContext[3] = (uint32)len;
    g_busContext[4] = 0u;
    g_busContext[5] = 0u;
    g_busCaptured = FALSE;
}

static void tof_captureBus(Ifx_I2C *i2c)
{
    uint32 i;
    if (g_busCaptured != FALSE)
    {
        return;
    }
    for (i = 0u; i < 4u; i++)
    {
        g_busDiag[i] = g_busContext[i];
    }
    g_busDiag[4] = i2c->PIRQSS.U;
    g_busDiag[5] = i2c->ERRIRQSS.U;
    g_busDiag[6] = i2c->RIS.U;
    g_busDiag[7] = (uint32)i2c->BUSSTAT.B.BS;
    g_busDiag[8] = (uint32)i2c->FFSSTAT.B.FFS;
    g_busDiag[9] = (IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) != FALSE) ? 1u : 0u;
    g_busDiag[10] = (IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) != FALSE) ? 1u : 0u;
    g_busDiag[11] = g_busContext[4]; /* TX packet bytes, including slave address */
    g_busDiag[12] = g_busContext[5]; /* slave address actually queued */
    g_busCaptured = TRUE;
    g_busDiagValid = TRUE;
}

static void tof_clearRequests(Ifx_I2C *i2c)
{
    IfxI2c_clearLastSingleRequestInterruptSource(i2c);
    IfxI2c_clearSingleRequestInterruptSource(i2c);
    IfxI2c_clearLastBurstRequestInterruptSource(i2c);
    IfxI2c_clearBurstRequestInterruptSource(i2c);
}

static TofStatus tof_protocolStatus(Ifx_I2C *i2c);

/* Wait for one protocol service flag and consume it.
 *
 * Never call this with the interrupts disabled. The deadline is a tick count,
 * and inside an interrupt-off window the tick cannot advance, so the wait could
 * never expire. All FIFO service waits also run with interrupts enabled. */
static TofStatus tof_waitProtocol(Ifx_I2C *i2c, IfxI2c_ProtocolInterruptSource source)
{
    uint32 startMs = tof_nowMs();
    if (g_busContext[0] != 7u)
    {
        g_busContext[0] = (source == IfxI2c_ProtocolInterruptSource_receiveMode) ? 5u : 4u;
    }

    while (IfxI2c_getProtocolInterruptSourceStatus(i2c, source) == FALSE)
    {
        TofStatus st = tof_protocolStatus(i2c);
        if (st != TOF_OK)
        {
            return st;
        }
        /* RX can arrive between the mode check and this loop body. Never
         * acknowledge its request here before the receive FIFO is drained. */
        if (source == IfxI2c_ProtocolInterruptSource_transmissionEnd)
        {
            tof_clearRequests(i2c);
        }
        if (tof_expired(startMs) != FALSE)
        {
            tof_captureBus(i2c);
            return TOF_ERR_TIMEOUT;
        }
    }

    IfxI2c_clearProtocolInterruptSource(i2c, source);
    return TOF_OK;
}

/* Arbitration lost, a NACK and a FIFO error flag are different bench stories:
 * arbitration lost reports SDA feedback differing from the transmitted bit;
 * it does not by itself prove a second master exists. NACK is the expected
 * answer while the module is absent or LPn is low, and a FIFO error flag says a
 * transaction outlived the FIFO that was feeding it. */
static TofStatus tof_protocolStatus(Ifx_I2C *i2c)
{
    if (IfxI2c_getProtocolInterruptSourceStatus(i2c,
            IfxI2c_ProtocolInterruptSource_arbitrationLost) == TRUE)
    {
        tof_captureBus(i2c);
        IfxI2c_clearProtocolInterruptSource(i2c,
            IfxI2c_ProtocolInterruptSource_arbitrationLost);
        return TOF_ERR_BUS;
    }
    if (IfxI2c_getProtocolInterruptSourceStatus(i2c,
            IfxI2c_ProtocolInterruptSource_notAcknowledgeReceived) == TRUE)
    {
        tof_captureBus(i2c);
        IfxI2c_clearProtocolInterruptSource(i2c,
            IfxI2c_ProtocolInterruptSource_notAcknowledgeReceived);
        return TOF_ERR_NO_ACK;
    }
    if (i2c->ERRIRQSS.U != 0u)
    {
        tof_captureBus(i2c);
        IfxI2c_clearAllErrorInterruptSources(i2c);
        return TOF_ERR_BUS;
    }
    return TOF_OK;
}

/* TXFC/RXFC request handshake, with the scheduler and tick still running.
 * Each FIFO word needs its request acknowledged before the next request can
 * appear. Waiting for TX_END first deadlocks even the one-byte address probe. */
static TofStatus tof_waitRequest(Ifx_I2C *i2c)
{
    uint32 startMs = tof_nowMs();
    const uint32 mask = (1u << IFX_I2C_RIS_LSREQ_INT_OFF)
                      | (1u << IFX_I2C_RIS_SREQ_INT_OFF)
                      | (1u << IFX_I2C_RIS_LBREQ_INT_OFF)
                      | (1u << IFX_I2C_RIS_BREQ_INT_OFF);

    for (;;)
    {
        TofStatus st = tof_protocolStatus(i2c);
        if (st != TOF_OK)
        {
            return st;
        }
        if ((i2c->RIS.U & mask) != 0u)
        {
            return TOF_OK;
        }
        if (tof_expired(startMs) != FALSE)
        {
            tof_captureBus(i2c);
            return TOF_ERR_TIMEOUT;
        }
    }
}

/* Pack each FIFO-sized packet before a bounded register-only critical section.
 * No wait or wire transfer runs with interrupts disabled. */
static TofStatus tof_txFill(Ifx_I2C *i2c, const uint8 *buf, uint16 count)
{
    union
    {
        uint32 word;
        uint8  byte[4];
    } txData;
    uint32 words;
    uint32 i;
    uint32 packed[8];
    uint32 startMs;
    boolean interruptsEnabled;

    if ((count == 0u) || (count > TOF_TX_MAX))
    {
        return TOF_ERR_PARAM;
    }
    g_busContext[4] = (uint32)count;
    g_busContext[5] = (uint32)buf[0];
    words = ((uint32)count + 3u) / 4u;
    for (i = 0u; i < words; i++)
    {
        uint32 k;
        txData.word = 0u;
        for (k = 0u; k < 4u; k++)
        {
            uint32 index = (i * 4u) + k;
            if (index < (uint32)count)
            {
                txData.byte[k] = buf[index];
            }
        }
        packed[i] = txData.word;
    }
    /* TC27x I2C_TC.001: drain the previous packet before programming TPS. */
    startMs = tof_nowMs();
    g_busContext[0] = 2u;
    while (i2c->FFSSTAT.B.FFS != 0u)
    {
        TofStatus st = tof_protocolStatus(i2c);
        if (st != TOF_OK)
        {
            return st;
        }
        if (tof_expired(startMs) != FALSE)
        {
            tof_captureBus(i2c);
            return TOF_ERR_TIMEOUT;
        }
    }
    g_busContext[0] = 3u;
    interruptsEnabled = IfxCpu_disableInterrupts();
    IfxI2c_setTransmitPacketSize(i2c, (Ifx_SizeT)count);
    for (i = 0u; i < words; i++)
    {
        IfxI2c_writeFifo(i2c, packed[i]);
        tof_clearRequests(i2c);
    }
    IfxCpu_restoreInterrupts(interruptsEnabled);
    return tof_protocolStatus(i2c);
}

/* STOP, with a bound. IfxI2c_releaseBus does the identical SETEND write and then
 * waits for TX_END forever; a stuck end-of-transmission has to become
 * TOF_ERR_TIMEOUT and a module reset, not a task that never returns. */
static TofStatus tof_endSession(Ifx_I2C *i2c)
{
    TofStatus st;

    if (i2c->BUSSTAT.B.BS == IfxI2c_BusStatus_idle)
    {
        return TOF_OK;
    }

    g_busContext[0] = 7u; /* STOP */
    i2c->ENDDCTRL.B.SETEND = 1;

    st = tof_waitProtocol(i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
    if (st != TOF_OK)
    {
        return st;
    }
    return tof_protocolStatus(i2c);
}

/* One register write: address + index pair + payload, then STOP.
 * At most one message, because a message is capped at
 * the FIFO size and the caller (WrMulti) does the chunking. */
static TofStatus tof_writeReg(uint8 addr8, uint16 reg, const uint8 *data, uint16 len)
{
    Ifx_I2C  *i2c = TOF_I2C;
    TofStatus st;
    TofStatus endSt;
    uint16    msgLen;
    uint16    i;

    if ((len == 0u) || (len > TOF_WRITE_CHUNK) || (data == NULL_PTR))
    {
        return TOF_ERR_PARAM;
    }
    msgLen = TOF_txMessageBytes(len);
    if ((msgLen > TOF_TX_MAX) || (TOF_indexFits(reg, (uint32)len) == FALSE))
    {
        return TOF_ERR_PARAM;
    }

    tof_busBegin(FALSE, reg, len);
    if (IfxI2c_busIsFree(i2c) == FALSE)
    {
        tof_captureBus(i2c);
        return TOF_ERR_BUS;
    }
    IfxI2c_clearAllProtocolInterruptSources(i2c);
    IfxI2c_clearAllErrorInterruptSources(i2c);

    g_txBuf[0] = TOF_slaveAddrWrite(addr8);
    g_txBuf[1] = TOF_wireAddrHiWrite(reg);
    g_txBuf[2] = TOF_wireAddrLo(reg);
    for (i = 0u; i < len; i++)
    {
        g_txBuf[3u + i] = data[i];
    }
    st = tof_txFill(i2c, g_txBuf, msgLen);
    if (st == TOF_OK)
    {
        st = tof_waitProtocol(i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
    }
    if (st == TOF_OK)
    {
        st = tof_protocolStatus(i2c);
    }
    tof_clearRequests(i2c);

    endSt = tof_endSession(i2c);
    if ((st == TOF_OK) && (endSt != TOF_OK))
    {
        st = endSt;
    }
    return st;
}

/* Register read: index write, repeated START + address/R, RX mode,
 * then consume/acknowledge each FIFO request before waiting for TX_END.
 * Waiting for TX_END before servicing RXFC can stall a multi-word read. */
static TofStatus tof_readReg(uint8 addr8, uint16 reg, uint8 *buf, uint16 len)
{
    Ifx_I2C  *i2c = TOF_I2C;
    TofStatus st;
    TofStatus endSt;
    uint32    words;
    uint32    i;

    if ((buf == NULL_PTR) || (len == 0u) || (len > TOF_READ_CHUNK))
    {
        return TOF_ERR_PARAM;
    }
    if (TOF_indexFits(reg, (uint32)len) == FALSE)
    {
        return TOF_ERR_PARAM;
    }

    tof_busBegin(TRUE, reg, len);
    if (IfxI2c_busIsFree(i2c) == FALSE)
    {
        tof_captureBus(i2c);
        (void)memset(buf, 0, (size_t)len);
        return TOF_ERR_BUS;
    }
    IfxI2c_clearAllProtocolInterruptSources(i2c);
    IfxI2c_clearAllErrorInterruptSources(i2c);

    /* 1. point the device at the index: address + 2-byte index, no payload */
    g_txBuf[0] = TOF_slaveAddrWrite(addr8);
    g_txBuf[1] = TOF_wireAddrHiWrite(reg);
    g_txBuf[2] = TOF_wireAddrLo(reg);
    st = tof_txFill(i2c, g_txBuf, TOF_txMessageBytes(0u));
    if (st == TOF_OK)
    {
        st = tof_waitProtocol(i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
    }
    if (st == TOF_OK)
    {
        st = tof_protocolStatus(i2c);
    }
    tof_clearRequests(i2c);

    /* 2. repeated START with RnW in the slave address. Service the TX request,
     * wait for RX mode, then drain the RX requests before waiting for TX_END. */
    if (st == TOF_OK)
    {
        IfxI2c_setReceivePacketSize(i2c, (Ifx_SizeT)len);

        g_txBuf[0] = TOF_slaveAddrRead(addr8);
        st = tof_txFill(i2c, g_txBuf, 1u);
        if (st == TOF_OK)
        {
            st = tof_waitProtocol(i2c, IfxI2c_ProtocolInterruptSource_receiveMode);
        }

        if (st == TOF_OK)
        {
            words = ((uint32)len + 3u) / 4u;

            for (i = 0u; i < words; i++)
            {
                union
                {
                    uint32 word;
                    uint8  byte[4];
                } rxData;
                uint32 remain = (uint32)len - (i * 4u);
                uint32 bytes  = (remain >= 4u) ? 4u : remain;
                uint32 k;

                g_busContext[0] = 6u; /* RX request */
                st = tof_waitRequest(i2c);
                if (st != TOF_OK)
                {
                    break;
                }
                rxData.word = i2c->RXD.U;
                tof_clearRequests(i2c);

                for (k = 0u; k < bytes; k++)
                {
                    buf[(i * 4u) + k] = rxData.byte[k];
                }
            }
        }
    }

    if (st == TOF_OK)
    {
        st = tof_waitProtocol(i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
    }

    endSt = tof_endSession(i2c);
    if ((st == TOF_OK) && (endSt != TOF_OK))
    {
        st = endSt;
    }
    if (st != TOF_OK)
    {
        /* The ULD compares these bytes even on failure. Never return stale
         * identity bytes or partial data, including a failed STOP. */
        (void)memset(buf, 0, (size_t)len);
    }
    return st;
}

/* Module + pads, from a known state. Recovery also clocks a stuck slave:
 * resetting I2C0 alone does not release an externally held SDA. */
static void tof_busSetup(void)
{
    Ifx_I2C *i2c = TOF_I2C;

    /* Order copied from IfxI2c_I2c_initModule: enable, stop (config mode),
     * master, baudrate, pads, run. IfxI2c_configureAsMaster already leaves the
     * FIFO in the state that driver's own default config asks for (TX and RX
     * flow control on, burst 1 word, byte aligned) plus ADDRCFG.SOPE = 0, so no
     * IfxI2c_configureAddrFifo call is needed. enableModule, setBaudrate and
     * resetModule each take the CPU watchdog ENDINIT password internally. */
    IfxI2c_enableModule(i2c);
    IfxI2c_stop(i2c);
    IfxI2c_configureAsMaster(i2c);
    IfxI2c_setBaudrate(i2c, TOF_I2C_BAUDRATE);

    /* Both pads open-drain, which is what initSclSdaPin does - and the reason
     * doc 23 section 11.1 makes the 2.2 k pull-ups a requirement instead of a
     * suggestion. Speed1 keeps the edges slow on jumper wires, the same pad
     * driver discipline as the IMU's CS. */
    IfxI2c_initSclSdaPin(&IfxI2c0_SCL_P02_5_INOUT, &IfxI2c0_SDA_P02_4_INOUT,
                         IfxPort_PadDriver_cmosAutomotiveSpeed1);

    IfxI2c_run(i2c);

    /* Post-divider value: what the wire actually does, not what was asked for.
     * The bench compares it against a scope reading (doc 23 section 11.4 step 5)
     * - the IMU batch reported the requested clock instead of the measured one,
     * and I2C0 is a different module whose divider that lesson cannot cover. */
    g_actualHz = IfxI2c_getBaudrate(i2c);
}

/* UM10204 bus clear: GPIO open-drain only, never drive a line high.
 * A peripheral reset cannot free a slave holding SDA after an aborted read.
 * result: 0 already idle, 1 cleared, 2 SCL held low, 3 SDA still held low. */
static uint32 g_busClear[4]; /* result / pulse count / final SCL / final SDA */

static uint32 tof_clearBusPins(void)
{
    uint32 pulse;
    uint32 result = 0u;
    g_busClear[1] = 0u;

    IfxPort_setPinHigh(TOF_SCL_PORT, TOF_SCL_PIN);
    IfxPort_setPinHigh(TOF_SDA_PORT, TOF_SDA_PIN);
    IfxPort_setPinModeOutput(TOF_SCL_PORT, TOF_SCL_PIN, IfxPort_OutputMode_openDrain, IfxPort_OutputIdx_general);
    IfxPort_setPinModeOutput(TOF_SDA_PORT, TOF_SDA_PIN, IfxPort_OutputMode_openDrain, IfxPort_OutputIdx_general);
    vTaskDelay(pdMS_TO_TICKS(1u));

    if (IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) == FALSE)
    {
        result = 2u;
    }
    else if (IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) == FALSE)
    {
        for (pulse = 0u; pulse < 9u; pulse++)
        {
            IfxPort_setPinLow(TOF_SCL_PORT, TOF_SCL_PIN);
            vTaskDelay(pdMS_TO_TICKS(1u));
            IfxPort_setPinHigh(TOF_SCL_PORT, TOF_SCL_PIN);
            vTaskDelay(pdMS_TO_TICKS(1u));
            g_busClear[1]++;
            if (IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) == FALSE)
            {
                result = 2u;
                break;
            }
            if (IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) != FALSE)
            {
                break;
            }
        }
        if (result != 2u)
        {
            /* STOP: establish SDA low with SCL low, then release SCL/SDA. */
            IfxPort_setPinLow(TOF_SCL_PORT, TOF_SCL_PIN);
            IfxPort_setPinLow(TOF_SDA_PORT, TOF_SDA_PIN);
            vTaskDelay(pdMS_TO_TICKS(1u));
            IfxPort_setPinHigh(TOF_SCL_PORT, TOF_SCL_PIN);
            vTaskDelay(pdMS_TO_TICKS(1u));
            if (IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) == FALSE)
            {
                result = 2u;
            }
            IfxPort_setPinHigh(TOF_SDA_PORT, TOF_SDA_PIN);
            vTaskDelay(pdMS_TO_TICKS(1u));
            if (result != 2u)
            {
                result = (IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) != FALSE) ? 1u : 3u;
            }
        }
    }
    IfxPort_setPinHigh(TOF_SCL_PORT, TOF_SCL_PIN);
    IfxPort_setPinHigh(TOF_SDA_PORT, TOF_SDA_PIN);
    g_busClear[0] = result;
    g_busClear[2] = (IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) != FALSE) ? 1u : 0u;
    g_busClear[3] = (IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) != FALSE) ? 1u : 0u;
    return result;
}

/* One boot-only GPIO address probe, independent of the I2C0 kernel.
 * Sends address/W only, reads its ACK, then STOP; no register or data writes.
 * status / sampled address bits / ACK bit / first differing bit (1..8).
 * status: 0 ACK, 1 NACK, 2 SCL held, 3 SDA held, 4 address contention. */
static void tof_gpioProbe(void)
{
    uint32 vals[4] = {0u, 0u, 1u, 0u};
    uint32 clear;
    uint32 bit;
    IfxI2c_stop(TOF_I2C);
    clear = tof_clearBusPins();
    if ((clear == 2u) || (clear == 3u))
    {
        vals[0] = clear;
    }
    else
    {
        IfxPort_setPinLow(TOF_SDA_PORT, TOF_SDA_PIN); /* START, SCL already released */
        vTaskDelay(pdMS_TO_TICKS(1u));
        for (bit = 0u; bit < 8u; bit++)
        {
            uint32 sent = ((uint32)TOF_I2C_ADDRESS >> (7u - bit)) & 1u;
            uint32 seen;
            IfxPort_setPinLow(TOF_SCL_PORT, TOF_SCL_PIN);
            /* Let the physical open-drain SCL fall before changing SDA.
             * Back-to-back port writes can otherwise look like a START/STOP
             * to the slave when the two jumper-wire edges settle differently. */
            vTaskDelay(pdMS_TO_TICKS(1u));
            if (sent != 0u)
            {
                IfxPort_setPinHigh(TOF_SDA_PORT, TOF_SDA_PIN);
            }
            else
            {
                IfxPort_setPinLow(TOF_SDA_PORT, TOF_SDA_PIN);
            }
            vTaskDelay(pdMS_TO_TICKS(1u));
            IfxPort_setPinHigh(TOF_SCL_PORT, TOF_SCL_PIN);
            vTaskDelay(pdMS_TO_TICKS(1u));
            if (IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) == FALSE)
            {
                vals[0] = 2u;
                break;
            }
            seen = (IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) != FALSE) ? 1u : 0u;
            vals[1] = (vals[1] << 1u) | seen;
            if ((sent != seen) && (vals[3] == 0u))
            {
                vals[0] = 4u;
                vals[3] = bit + 1u;
                break; /* release a contended bus instead of continuing */
            }
        }
        if (vals[0] == 0u)
        {
            IfxPort_setPinLow(TOF_SCL_PORT, TOF_SCL_PIN);
            vTaskDelay(pdMS_TO_TICKS(1u));
            IfxPort_setPinHigh(TOF_SDA_PORT, TOF_SDA_PIN); /* release for slave ACK */
            vTaskDelay(pdMS_TO_TICKS(1u));
            IfxPort_setPinHigh(TOF_SCL_PORT, TOF_SCL_PIN);
            vTaskDelay(pdMS_TO_TICKS(1u));
            if (IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) == FALSE)
            {
                vals[0] = 2u;
            }
            else
            {
                vals[2] = (IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) != FALSE) ? 1u : 0u;
                vals[0] = vals[2];
            }
        }
        IfxPort_setPinLow(TOF_SCL_PORT, TOF_SCL_PIN);
        vTaskDelay(pdMS_TO_TICKS(1u));
        IfxPort_setPinLow(TOF_SDA_PORT, TOF_SDA_PIN);
        vTaskDelay(pdMS_TO_TICKS(1u));
        IfxPort_setPinHigh(TOF_SCL_PORT, TOF_SCL_PIN);
        vTaskDelay(pdMS_TO_TICKS(1u));
        IfxPort_setPinHigh(TOF_SDA_PORT, TOF_SDA_PIN); /* STOP if clock released */
        vTaskDelay(pdMS_TO_TICKS(1u));
    }
    XCORE_LOG_FIELDS("[TOF_GPIO]", XL_U("status_raw", vals[0]), XL_H("address_echo", vals[1]), XL_U("ack_line_level", vals[2]), XL_U("first_different_bit", vals[3]));
    tof_busSetup(); /* restore peripheral pin selection and clock */
}

#if (TOF_GPIO_ISOLATION != 0)
static void tof_gpioIsolation(void)
{
    static const uint8 states[5][2] = {{1u,1u},{1u,0u},{1u,1u},{0u,1u},{1u,1u}};
    uint32 step;
    IfxI2c_stop(TOF_I2C);
    IfxPort_setPinHigh(TOF_SCL_PORT, TOF_SCL_PIN);
    IfxPort_setPinHigh(TOF_SDA_PORT, TOF_SDA_PIN);
    IfxPort_setPinModeOutput(TOF_SCL_PORT, TOF_SCL_PIN, IfxPort_OutputMode_openDrain, IfxPort_OutputIdx_general);
    IfxPort_setPinModeOutput(TOF_SDA_PORT, TOF_SDA_PIN, IfxPort_OutputMode_openDrain, IfxPort_OutputIdx_general);
    for (step = 0u; step < 5u; step++)
    {
        uint32 vals[10];
        IfxPort_setPinState(TOF_SCL_PORT, TOF_SCL_PIN, states[step][0] ? IfxPort_State_high : IfxPort_State_low);
        vTaskDelay(pdMS_TO_TICKS(1u));
        IfxPort_setPinState(TOF_SDA_PORT, TOF_SDA_PIN, states[step][1] ? IfxPort_State_high : IfxPort_State_low);
        vTaskDelay(pdMS_TO_TICKS(1000u));
        vals[0] = step;
        vals[1] = states[step][0];
        vals[2] = states[step][1];
        vals[3] = IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) ? 1u : 0u;
        vals[4] = IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) ? 1u : 0u;
        vals[5] = TOF_SDA_PORT->IN.U;
        vals[6] = TOF_SDA_PORT->OUT.U;
        vals[7] = TOF_SDA_PORT->IOCR4.U;
        vals[8] = TOF_I2C->GPCTL.U;
        vals[9] = TOF_I2C->RUNCTRL.U;
        XCORE_LOG_FIELDS("[TOF_ISOLATION]", XL_U("step", vals[0]), XL_U("requested_scl", vals[1]), XL_U("requested_sda", vals[2]), XL_U("scl_level", vals[3]), XL_U("sda_level", vals[4]), XL_H("port_in", vals[5]), XL_H("port_out", vals[6]), XL_H("iocr4", vals[7]), XL_H("gpctl", vals[8]), XL_H("runctrl", vals[9]));
    }
    /* Compare the external bias with a known internal weak pull-up after
     * discharging SDA. Keep SCL low so these are not START/STOP edges. */
    IfxPort_setPinLow(TOF_SCL_PORT, TOF_SCL_PIN);
    vTaskDelay(pdMS_TO_TICKS(1u));
    for (step = 0u; step < 2u; step++)
    {
        uint32 vals[5];
        IfxPort_setPinLow(TOF_SDA_PORT, TOF_SDA_PIN);
        IfxPort_setPinModeOutput(TOF_SDA_PORT, TOF_SDA_PIN, IfxPort_OutputMode_openDrain, IfxPort_OutputIdx_general);
        vTaskDelay(pdMS_TO_TICKS(100u));
        IfxPort_setPinHigh(TOF_SDA_PORT, TOF_SDA_PIN);
        IfxPort_setPinModeInput(TOF_SDA_PORT, TOF_SDA_PIN, step ? IfxPort_InputMode_pullUp : IfxPort_InputMode_noPullDevice);
        vTaskDelay(pdMS_TO_TICKS(1000u));
        vals[0] = step;
        vals[1] = IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) ? 1u : 0u;
        vals[2] = IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) ? 1u : 0u;
        vals[3] = TOF_SDA_PORT->IN.U;
        vals[4] = TOF_SDA_PORT->IOCR4.U;
        XCORE_LOG_FIELDS("[TOF_BIAS]", XL_U("internal_pull_up", vals[0]), XL_U("scl_level", vals[1]), XL_U("sda_level", vals[2]), XL_H("port_in", vals[3]), XL_H("iocr4", vals[4]));
    }
    IfxPort_setPinHigh(TOF_SDA_PORT, TOF_SDA_PIN);
    IfxPort_setPinModeInput(TOF_SDA_PORT, TOF_SDA_PIN, IfxPort_InputMode_pullUp);
    vTaskDelay(pdMS_TO_TICKS(1u));
    IfxPort_setPinHigh(TOF_SCL_PORT, TOF_SCL_PIN);
    IfxPort_setPinModeInput(TOF_SCL_PORT, TOF_SCL_PIN, IfxPort_InputMode_pullUp);
    g_state = TOF_ST_DEAD;
    XCORE_logln("TOFISO parked: I2C0 stopped; P02.5/P02.4 weak pull-up inputs; ranging disabled");
}
#endif

static void tof_recover(void)
{
    IfxI2c_resetModule(TOF_I2C);
    IfxI2c_stop(TOF_I2C);
    (void)tof_clearBusPins();
    tof_busSetup();
}

/* ---- ST platform layer: the six functions platform.h promises ---- */

/* Every bus failure funnels through here. The counters are the bench evidence, a
 * stalled transaction gets a module reset, and the value the ULD wants for an
 * MCU-side failure is VL53L5CX_MCU_ERROR - it OR-s whatever the callbacks return
 * straight into its own status. */
static uint8_t tof_failed(TofStatus st)
{
    g_lastErr  = st;
    g_errCount++;

    if ((st == TOF_ERR_TIMEOUT) || (st == TOF_ERR_BUS))
    {
        tof_recover();
    }
    return VL53L5CX_MCU_ERROR;
}

uint8_t RdByte(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t *p_value)
{
    TofStatus st;

    st = tof_readReg((uint8)p_platform->address, (uint16)RegisterAdress, p_value, 1u);
    return (st == TOF_OK) ? (uint8_t)VL53L5CX_STATUS_OK : tof_failed(st);
}

uint8_t WrByte(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t value)
{
    TofStatus st;
    uint8     payload = value;    /* its own storage: g_txBuf is the frame
                                   * staging buffer tof_writeReg composes in */

    st = tof_writeReg((uint8)p_platform->address, (uint16)RegisterAdress, &payload, 1u);
    return (st == TOF_OK) ? (uint8_t)VL53L5CX_STATUS_OK : tof_failed(st);
}

/* Bursts are cut into FIFO-sized messages, each carrying its own explicit
 * index. That is the point: nothing here relies on the device continuing to
 * auto-increment across a transaction boundary, so the split is invisible to the
 * sensor's register protocol. TOF_indexFits is checked once, before the first
 * message, so a burst that would run past the 16-bit index space inside the
 * current page cannot half-send - it fails as a parameter error instead. A size
 * of zero produces no messages and is reported as success. */
uint8_t RdMulti(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress,
                uint8_t *p_values, uint32_t size)
{
    uint8  addr8 = (uint8)p_platform->address;
    uint16 base  = (uint16)RegisterAdress;
    uint32 chunks;
    uint32 c;

    if ((TOF_indexFits(base, size) == FALSE) || ((p_values == NULL_PTR) && (size > 0u)))
    {
        return tof_failed(TOF_ERR_PARAM);
    }

    chunks = TOF_chunkCount(size, TOF_READ_CHUNK);
    for (c = 0u; c < chunks; c++)
    {
        TofStatus st = tof_readReg(addr8,
                                   TOF_chunkAddr(base, c, TOF_READ_CHUNK),
                                   &p_values[c * (uint32)TOF_READ_CHUNK],
                                   TOF_chunkLen(c, size, TOF_READ_CHUNK));
        if (st != TOF_OK)
        {
            return tof_failed(st);
        }
    }
    return (uint8_t)VL53L5CX_STATUS_OK;
}

uint8_t WrMulti(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress,
                uint8_t *p_values, uint32_t size)
{
    uint8  addr8 = (uint8)p_platform->address;
    uint16 base  = (uint16)RegisterAdress;
    uint32 chunks;
    uint32 c;

    if ((TOF_indexFits(base, size) == FALSE) || ((p_values == NULL_PTR) && (size > 0u)))
    {
        return tof_failed(TOF_ERR_PARAM);
    }

    chunks = TOF_chunkCount(size, TOF_WRITE_CHUNK);
    for (c = 0u; c < chunks; c++)
    {
        TofStatus st = tof_writeReg(addr8,
                                    TOF_chunkAddr(base, c, TOF_WRITE_CHUNK),
                                    &p_values[c * (uint32)TOF_WRITE_CHUNK],
                                    TOF_chunkLen(c, size, TOF_WRITE_CHUNK));
        if (st != TOF_OK)
        {
            return tof_failed(st);
        }
    }
    return (uint8_t)VL53L5CX_STATUS_OK;
}

/* Sensor words are big-endian and TriCore is little-endian, so every 4-byte
 * group is reversed in place. Unconditional, like the vendor's reference
 * implementation: the ULD calls this both on the result page it has just read
 * and on the DCI buffers it is about to write in wire order, and the operation
 * is its own inverse. */
void SwapBuffer(uint8_t *buffer, uint16_t size)
{
    uint16 i;

    for (i = 0u; ((uint32)i + 3u) < (uint32)size; i += 4u)
    {
        uint8_t b0 = buffer[i];
        uint8_t b1 = buffer[(uint16)(i + 1u)];

        buffer[i]                = buffer[(uint16)(i + 3u)];
        buffer[(uint16)(i + 1u)] = buffer[(uint16)(i + 2u)];
        buffer[(uint16)(i + 2u)] = b1;
        buffer[(uint16)(i + 3u)] = b0;
    }
}

uint8_t WaitMs(VL53L5CX_Platform *p_platform, uint32_t TimeMs)
{
    (void)p_platform;

    /* Yield, do not spin. vl53l5cx_init waits 100 ms once and the ULD's
     * poll-for-answer loop waits in 10 ms steps for up to 2 s; the reference
     * implementation busy-waits through all of that, which on this core would
     * starve the same-priority blinky and echo tasks for seconds at boot for no
     * reason. The robot task is unaffected either way - it preempts us. */
    vTaskDelay(pdMS_TO_TICKS((TickType_t)TimeMs));
    return (uint8_t)VL53L5CX_STATUS_OK;
}

/* ---- bring-up ladder ---- */

/* A non-zero status out of the ULD is either a bus failure the platform layer
 * has already counted (it comes back as VL53L5CX_MCU_ERROR) or the sensor
 * refusing a command. g_cfgStep says which ladder step, g_stStatus with what
 * code: together they are the difference between "wiring is wrong" and "wiring
 * is fine, the part will not accept the settings". */
static TofStatus tof_stFailed(uint8_t st)
{
    g_stStatus = (uint8)st;
    return TOF_ERR_ST;
}

static void tof_logFailure(void)
{
    static const char *const errors[] = {"ok", "invalid_parameter", "no_ack", "bus_error", "timeout", "uld_error"};
    static const char *const phases[] = {"none", "bus_free", "tx_space", "tx_request", "tx_end", "rx_mode", "rx_request", "stop"};
    static const char *const clear[] = {"idle", "released", "scl_stuck", "sda_stuck"};
    g_errLogMs = tof_nowMs();
    XCORE_LOG_FIELDS("[TOF_ERROR]", XL_U("uptime_ms", g_errLogMs),
        XL_S("cause", (uint32)g_lastErr < 6u ? errors[g_lastErr] : "unknown"),
        XL_H("uld_status", g_stStatus), XL_U("config_step", g_cfgStep), XL_U("errors_total", g_errCount));
    if (g_busDiagValid != FALSE)
    {
        XCORE_LOG_FIELDS("[TOF_BUS]", XL_U("uptime_ms", g_errLogMs),
            XL_S("phase", g_busDiag[0] < 8u ? phases[g_busDiag[0]] : "unknown"),
            XL_S("direction", g_busDiag[1] ? "read" : "write"),
            XL_H("register", g_busDiag[2]), XL_U("length_bytes", g_busDiag[3]),
            XL_H("protocol_irq", g_busDiag[4]), XL_H("error_irq", g_busDiag[5]),
            XL_H("raw_irq", g_busDiag[6]), XL_U("bus_state_raw", g_busDiag[7]),
            XL_U("fifo_words", g_busDiag[8]), XL_U("scl_level", g_busDiag[9]), XL_U("sda_level", g_busDiag[10]),
            XL_U("packet_bytes", g_busDiag[11]), XL_H("wire_address", g_busDiag[12]));
        XCORE_LOG_FIELDS("[TOF_CLEAR]", XL_U("uptime_ms", g_errLogMs),
            XL_S("result", g_busClear[0] < 4u ? clear[g_busClear[0]] : "unknown"),
            XL_U("pulses", g_busClear[1]), XL_U("scl_level", g_busClear[2]), XL_U("sda_level", g_busClear[3]));
    }
}

/* The identity pair, read for the log only. The ULD's own vl53l5cx_is_alive
 * stays the accept/reject decision - the comparison never lives here (doc 23
 * section 11.4 step 4, the hand-copied-identity-byte lesson). This helper
 * exists because is_alive throws away the bytes it compared, and the bench
 * needs to see what actually answers: the IMU row has carried its WHOAMI for
 * exactly that reason since the 0x70/0x71 episode (doc 23 section 10.2). The
 * page dance is vl53l5cx_api.c:224-227 verbatim, run through the same
 * platform WrByte/RdByte is_alive itself uses. Called only where the bus is
 * proven to answer (probe passed or identity mismatched), so on a dead wire
 * g_whoAmI keeps reporting 0/0 - nothing came off it - and the failure rate
 * of the dead-wire state keeps its exact +4/s signature. */
static void tof_readWhoAmI(void)
{
    uint8_t id[2] = {0u, 0u};

    if (WrByte(&g_dev.platform, 0x7fff, 0x00) == (uint8_t)VL53L5CX_STATUS_OK)
    {
        (void)RdByte(&g_dev.platform, 0x0000, &id[0]);
        (void)RdByte(&g_dev.platform, 0x0001, &id[1]);
        (void)WrByte(&g_dev.platform, 0x7fff, 0x02);
    }

    g_whoAmI[0] = (uint8)id[0];
    g_whoAmI[1] = (uint8)id[1];
}

static TofStatus tof_configure(void)
{
    uint8_t st;

    /* Awake first: the DCI settings only land on a sensor out of sleep. */
    g_cfgStep = 1u;
    st = vl53l5cx_set_power_mode(&g_dev, VL53L5CX_POWER_MODE_WAKEUP);
    if (st != (uint8_t)VL53L5CX_STATUS_OK)
    {
        return tof_stFailed(st);
    }

    g_cfgStep = 2u;
    st = vl53l5cx_set_resolution(&g_dev, TOF_CFG_RESOLUTION);
    if (st != (uint8_t)VL53L5CX_STATUS_OK)
    {
        return tof_stFailed(st);
    }

    /* Mode before the integration time - in continuous mode the setting is
     * ignored, so writing it first would be a silent no-op. */
    g_cfgStep = 3u;
    st = vl53l5cx_set_ranging_mode(&g_dev, TOF_CFG_RANGING_MODE);
    if (st != (uint8_t)VL53L5CX_STATUS_OK)
    {
        return tof_stFailed(st);
    }

    g_cfgStep = 4u;
    st = vl53l5cx_set_integration_time_ms(&g_dev, (uint32_t)TOF_CFG_INTEGRATION_MS);
    if (st != (uint8_t)VL53L5CX_STATUS_OK)
    {
        return tof_stFailed(st);
    }

    g_cfgStep = 5u;
    st = vl53l5cx_set_target_order(&g_dev, TOF_CFG_TARGET_ORDER);
    if (st != (uint8_t)VL53L5CX_STATUS_OK)
    {
        return tof_stFailed(st);
    }

    g_cfgStep = 6u;
    st = vl53l5cx_set_ranging_frequency_hz(&g_dev, TOF_CFG_FREQ_HZ);
    if (st != (uint8_t)VL53L5CX_STATUS_OK)
    {
        return tof_stFailed(st);
    }

    g_cfgStep = 7u;
    st = vl53l5cx_start_ranging(&g_dev);
    if (st != (uint8_t)VL53L5CX_STATUS_OK)
    {
        return tof_stFailed(st);
    }

    return TOF_OK;
}

/* Closest zone carrying a valid measurement. This is a bench readout, not the
 * avoidance rule (that consumer is later work, doc 23 section 11.5): it exists
 * so doc 23 section 11.4 step 6 - "move a hand 10-30 cm in front and watch the
 * distance follow" - is checkable straight off the serial line, and it only
 * trusts the target statuses the vendor documents as OK. */
static sint16 tof_nearestMm(uint8 *outStatus)
{
    sint16 nearest = 0;
    uint8  status  = 0u;
    uint8  i;

    for (i = 0u; i < TOF_ZONE_COUNT; i++)
    {
        sint16 mm = g_results.distance_mm[i];

        if ((TOF_targetStatusOk(g_results.target_status[i]) == FALSE) || (mm <= 0))
        {
            continue;
        }
        if ((nearest == 0) || (mm < nearest))
        {
            nearest = mm;
            status  = g_results.target_status[i];
        }
    }

    *outStatus = status;
    return nearest;
}

static void tof_dataFailed(uint8_t st)
{
    g_stStatus = (uint8)st;

    g_failRun++;
    if (g_failRun < TOF_ALIVE_FAIL_LIMIT)
    {
        return;
    }

    g_failRun   = 0u;
    g_alive     = FALSE;
    g_snapshot.alive = 0u;
    XCORE_tofPublish(&g_snapshot);
    g_reprobeMs = tof_nowMs();
    /* Back to the identity check, which puts the firmware download back in the
     * ladder. That is the expensive part (~3 s of bus time) and it is the
     * correct one: a sensor that stopped answering is a sensor whose MCU may no
     * longer hold its firmware, and the vendor flow only guarantees the register
     * protocol after vl53l5cx_init. */
    g_state     = TOF_ST_PROBE;
    XCORE_logln("ToF bus lost (read failures), re-probing 1Hz");
}

static void tof_ladderFailed(void)
{
    /* Report at the same 0.5 Hz discipline as the TOF= row: a part that will not
     * come up must not consume the cross-core log ring at full rate. The reason
     * is already recorded - g_lastErr for a bus-level failure, g_stStatus and
     * g_cfgStep for a refusal by the sensor. */
    if ((uint32)(tof_nowMs() - g_errLogMs) >= TOF_LOG_PERIOD_MS)
    {
        tof_logFailure();
    }

    /* Any failed step leaves the sensor's state unknown, and the vendor contract
     * for getting it back to a known one is vl53l5cx_init. So recovery is the
     * whole ladder, throttled by the 1 Hz re-probe - including the ~3 s firmware
     * download, which is the expensive part and still the cheaper one compared to
     * guessing which setting survived. */
    g_failRun   = 0u;
    g_alive     = FALSE;
    g_snapshot.alive = 0u;
    XCORE_tofPublish(&g_snapshot);
    g_state     = TOF_ST_PROBE;
    g_reprobeMs = tof_nowMs();
}

static void tof_rangingStep(void)
{
    uint8_t ready = 0u;
    uint8_t st;

    /* Each poll is a 4-byte read (~0.15 ms of bus time). At a 5 ms task period
     * and 15 Hz of frames that is ~13 polls per frame - the cheap way to stay
     * responsive without depending on the INT line for anything. */
    st = vl53l5cx_check_data_ready(&g_dev, &ready);
    if (st != (uint8_t)VL53L5CX_STATUS_OK)
    {
        tof_dataFailed(st);
        return;
    }
    if (ready == 0u)
    {
        if ((uint32)(tof_nowMs() - g_frameMs) >= 1000u)
        {
            g_alive = FALSE;
            g_snapshot.alive = 0u;
            XCORE_tofPublish(&g_snapshot);
            g_state = TOF_ST_PROBE;
            g_reprobeMs = tof_nowMs();
            XCORE_logln("ToF frame timeout, re-probing");
        }
        return;                     /* frame not due yet */
    }

    st = vl53l5cx_get_ranging_data(&g_dev, &g_results);
    if (st != (uint8_t)VL53L5CX_STATUS_OK)
    {
        tof_dataFailed(st);
        return;
    }

    g_failRun = 0u;
    g_frameCount++;
    g_frameMs = tof_nowMs();
    g_snapshot.seq = g_frameCount;
    g_snapshot.stampMs = g_frameMs;
    g_snapshot.alive = 1u;
    g_snapshot.zones = TOF_ZONE_COUNT;
    {
        uint8 i;
        for (i = 0u; i < TOF_ZONE_COUNT; i++)
        {
            g_snapshot.distanceMm[i] = g_results.distance_mm[i];
            g_snapshot.status[i] = g_results.target_status[i];
            g_snapshot.targets[i] = g_results.nb_target_detected[i];
        }
    }
    XCORE_tofPublish(&g_snapshot);
}

void TOF_init(void)
{
    (void)memset(&g_dev, 0, sizeof(g_dev));

    /* The address is the only field the ULD needs from us. It is set here rather
     * than left to VL53L5CX_DEFAULT_I2C_ADDRESS because the platform struct is
     * the vendor's contract for it. The Write/Read/GetTick pointers stay NULL:
     * vl53l5cx_api.c never calls them (it calls the six free functions of
     * platform.h), and adapters for pointers nothing dereferences would be dead
     * code a later ULD drop-in still has to re-audit. */
    g_dev.platform.address = (uint16_t)TOF_I2C_ADDRESS;

    (void)memset(&g_results, 0, sizeof(g_results));
    (void)memset(&g_snapshot, 0, sizeof(g_snapshot));
    XCORE_tofPublish(&g_snapshot);

    g_state      = TOF_ST_PROBE;
    g_alive      = FALSE;
    g_cfgStep    = 0u;
    g_stStatus   = 0u;
    g_lastErr    = TOF_OK;
    g_errCount   = 0u;
    g_busCaptured = FALSE;
    g_busDiagValid = FALSE;
    g_failRun    = 0u;
    g_frameCount = 0u;
    g_initMs     = 0u;

    tof_busSetup();
#if (TOF_GPIO_ISOLATION != 0)
    tof_gpioIsolation();
#else
    tof_gpioProbe();
#endif
    XCORE_logln("ToF I2C0 SCL=P02.5(X304-7) SDA=P02.4(X304-8) 1MHz requested");

    /* Ordinary input, no internal pull device: the module drives INT open-drain
     * and the 47 k pull-up to IOVDD is the datasheet's own requirement (doc 23
     * section 11.1/11.3). Do NOT reach for IfxScuEru on this pin - ERU input
     * channel 0 belongs to the IMU (doc 23 section 11.2). */
    IfxPort_setPinModeInput(TOF_INT_PORT, TOF_INT_PIN, IfxPort_InputMode_noPullDevice);

    /* Unsigned wrap-safe arithmetic: this says "the first task pass probes
     * immediately". The ULD's own identity check is what decides alive. */
    g_reprobeMs = tof_nowMs() - TOF_REPROBE_MS;
    g_logMs     = tof_nowMs();
    g_errLogMs  = tof_nowMs() - TOF_LOG_PERIOD_MS;
}

void TOF_task(void)
{
    uint32 nowMs = tof_nowMs();

#if (TOF_GPIO_ISOLATION != 0)
    if ((uint32)(nowMs - g_logMs) >= TOF_LOG_PERIOD_MS)
    {
        uint32 vals[6];
        g_logMs = nowMs;
        vals[0] = IfxPort_getPinState(TOF_SCL_PORT, TOF_SCL_PIN) ? 1u : 0u;
        vals[1] = IfxPort_getPinState(TOF_SDA_PORT, TOF_SDA_PIN) ? 1u : 0u;
        vals[2] = TOF_SDA_PORT->IN.U;
        vals[3] = TOF_SDA_PORT->OUT.U;
        vals[4] = TOF_SDA_PORT->IOCR4.U;
        vals[5] = TOF_I2C->RUNCTRL.U;
        XCORE_LOG_FIELDS("[TOF_HOLD]", XL_U("uptime_ms", nowMs), XL_U("scl_level", vals[0]), XL_U("sda_level", vals[1]), XL_H("port_in", vals[2]), XL_H("port_out", vals[3]), XL_H("iocr4", vals[4]), XL_H("runctrl", vals[5]));
    }
    return;
#endif

    switch (g_state)
    {
    case TOF_ST_DEAD:
        break;

    case TOF_ST_PROBE:
        if ((uint32)(nowMs - g_reprobeMs) < TOF_REPROBE_MS)
        {
            break;
        }
        g_reprobeMs = nowMs;
        {
            uint8_t isAlive = 0u;
            uint8_t st;

            g_cfgStep = 0u;
            st = vl53l5cx_is_alive(&g_dev, &isAlive);

            if ((st == (uint8_t)VL53L5CX_STATUS_OK) && (isAlive == (uint8_t)1u))
            {
                /* vl53l5cx_is_alive is the vendor's own test (select page 0x7FFF
                 * -> 0, index 0 must read 0xF0, index 1 must read 0x02, page back
                 * to 0x02). Nothing here re-states those numbers: doc 23 section
                 * 11.4 step 4 exists precisely because the IMU batch hand-copied
                 * an identity byte once. */
                tof_readWhoAmI();       /* refresh the row's pair - the bus just
                                         * proved it answers */
                g_state = TOF_ST_INIT;
            }
            else if (st != (uint8_t)VL53L5CX_STATUS_OK)
            {
                /* Bus-level failure (NACK, timeout, FIFO flag): the platform
                 * layer has already counted and classified it. */
                (void)tof_stFailed(st);
                tof_ladderFailed();
            }
            else
            {
                /* Addressed cleanly but the identity did not match: a different
                 * part on the socket, or a module whose MCU has not booted.
                 * VL53L5CX_STATUS_ERROR with step 0 and berr untouched is what
                 * separates that from a wire fault, and the row keeps showing
                 * state=1: the bus works and the answer is wrong. tof_readWhoAmI
                 * is what makes the row say WHICH answer - the whole point of
                 * the IMU's WHOAMI precedent. */
                tof_readWhoAmI();
                g_stStatus = (uint8)VL53L5CX_STATUS_ERROR;
                tof_ladderFailed();
            }
        }
        break;

    case TOF_ST_INIT:
        {
            uint32  t0 = tof_nowMs();
            uint8_t st;

            g_cfgStep = 0u;
            st = vl53l5cx_init(&g_dev);

            /* Measured, not estimated: the 84 KB download is the number the bus
             * speed and the chunk size are judged by, and doc 36 section 6
             * compares the bench value against the ~2.2 s prediction. */
            g_initMs = (uint32)(tof_nowMs() - t0);

            if (st == (uint8_t)VL53L5CX_STATUS_OK)
            {
                g_state = TOF_ST_CONFIG;
            }
            else
            {
                (void)tof_stFailed(st);
                tof_ladderFailed();
            }
        }
        break;

    case TOF_ST_CONFIG:
        {
            TofStatus st = tof_configure();

            if (st == TOF_OK)
            {
                g_alive     = TRUE;
                g_frameMs   = tof_nowMs();
                g_failRun   = 0u;
                g_cfgStep   = 0u;
                g_stStatus  = 0u;
                g_lastErr   = TOF_OK;
                g_state     = TOF_ST_RANGING;
                XCORE_logln("ToF ranging (WHOAMI=0xF0/0x02, firmware loaded, settings applied)");
            }
            else
            {
                /* tof_configure() already recorded which step and which status. */
                tof_ladderFailed();
            }
        }
        break;

    case TOF_ST_RANGING:
        tof_rangingStep();
        break;

    default:
        break;
    }

    if ((uint32)(tof_nowMs() - g_logMs) >= TOF_LOG_PERIOD_MS)
    {
        static const char *const states[] = {"dead", "probe", "init", "config", "ranging"};
        uint8 nearestStatus = 0u;
        sint16 nearest = tof_nearestMm(&nearestStatus);
        g_logMs = tof_nowMs();
        XCORE_LOG_FIELDS("[TOF]", XL_U("uptime_ms", g_logMs),
            XL_S("state", (uint32)g_state < 5u ? states[g_state] : "unknown"),
            XL_U("alive", g_alive), XL_U("frames_total", g_frameCount), XL_U("errors_total", g_errCount),
            XL_I("nearest_mm", nearest), XL_U("target_status_raw", nearestStatus), XL_U("int_level", TOF_intLevel()));
        XCORE_LOG_FIELDS("[TOF_CONFIG]", XL_U("uptime_ms", g_logMs), XL_U("init_ms", g_initMs),
            XL_U("i2c_hz", g_actualHz), XL_U("zones", TOF_ZONE_COUNT),
            XL_H("device_id", g_whoAmI[0]), XL_H("revision_id", g_whoAmI[1]));
        /* Keep raw zones visible at the summary cadence: a close floor/body
         * return and an invalid optical return need different remedies. Raw
         * zone indices are sensor order, not calibrated vehicle directions. */
        if (g_alive && g_frameCount != 0u)
        {
            uint8 zone;
            for (zone = 0u; zone < TOF_ZONE_COUNT; zone++)
            {
                XCORE_LOG_FIELDS("[TOF_ZONE]", XL_U("frame", g_frameCount),
                    XL_U("zone", zone), XL_I("distance_mm", g_results.distance_mm[zone]),
                    XL_U("status_raw", g_results.target_status[zone]),
                    XL_U("targets", g_results.nb_target_detected[zone]));
            }
        }
    }
}

/* ---- getters ---- */

boolean TOF_isAlive(void)
{
    return g_alive;
}

uint32 TOF_errCount(void)
{
    return g_errCount;
}

uint32 TOF_frameCount(void)
{
    return g_frameCount;
}

uint32 TOF_initMs(void)
{
    return g_initMs;
}

uint16 TOF_zoneCount(void)
{
    return (uint16)TOF_ZONE_COUNT;
}

sint16 TOF_distanceMm(uint8 zone)
{
    if (zone >= TOF_ZONE_COUNT)
    {
        return 0;
    }
    return g_results.distance_mm[zone];
}

uint8 TOF_targetStatus(uint8 zone)
{
    if (zone >= TOF_ZONE_COUNT)
    {
        return 0u;
    }
    return g_results.target_status[zone];
}

uint8 TOF_intLevel(void)
{
    /* Level only, by design (doc 23 section 11.2): 1 while the module is
     * signalling a pending frame, 0 once the result has been read. */
    return (IfxPort_getPinState(TOF_INT_PORT, TOF_INT_PIN) != FALSE) ? 1u : 0u;
}
