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
 * (SCL=P13.1=mikroBUS hole 12, SDA=P13.2=hole 11), INT=P10.7=hole 15, AVDD and
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

/* 400 kHz is the design point and, for now, a ceiling: FM+ (1 MHz) requires
 * SCL/SDA tR <= 120 ns, which jumper wires behind 2.2 k pull-ups are not known
 * to meet until a scope has measured them (doc 23 section 11.3). */
#define TOF_I2C_BAUDRATE        400000.0f

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
#define TOF_LOG_PERIOD_MS       2000u

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
 * phase/rw/reg/len/PIRQSS/ERRIRQSS/RIS/BS/FFS/SCL/SDA, printed only at TOFERR cadence. */
static uint32 g_busDiag[11];
static uint32 g_busContext[4];
static boolean g_busCaptured;
static boolean g_busDiagValid;

static void tof_busBegin(boolean read, uint16 reg, uint16 len)
{
    g_busContext[0] = 1u; /* bus-free check */
    g_busContext[1] = (read != FALSE) ? 1u : 0u;
    g_busContext[2] = (uint32)reg;
    g_busContext[3] = (uint32)len;
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
    g_busDiag[9] = (IfxPort_getPinState(&MODULE_P13, 1u) != FALSE) ? 1u : 0u;
    g_busDiag[10] = (IfxPort_getPinState(&MODULE_P13, 2u) != FALSE) ? 1u : 0u;
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
        if (tof_expired(startMs) != FALSE)
        {
            tof_captureBus(i2c);
            return TOF_ERR_TIMEOUT;
        }
    }

    IfxI2c_clearProtocolInterruptSource(i2c, source);
    return TOF_OK;
}

/* Arbitration lost, a NACK and a FIFO error flag are three different bench
 * stories and must not collapse into one "bus error": arbitration lost says a
 * second master is on the net (there must not be one), NACK is the expected
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

/* Same per-word handshake as iLLD, but all waits are bounded and interruptible.
 * CPU0 is the sole owner; no interrupt handler accesses this FIFO. */
static TofStatus tof_txFill(Ifx_I2C *i2c, const uint8 *buf, uint16 count)
{
    union
    {
        uint32 word;
        uint8  byte[4];
    } txData;
    uint32 words;
    uint32 i;

    if ((count == 0u) || (count > TOF_TX_MAX))
    {
        return TOF_ERR_PARAM;
    }
    words = ((uint32)count + 3u) / 4u;
    IfxI2c_setTransmitPacketSize(i2c, (Ifx_SizeT)count);

    for (i = 0u; i < words; i++)
    {
        uint32 k;
        TofStatus st;
        uint32 startMs = tof_nowMs();
        g_busContext[0] = 2u; /* TX FIFO space */
        while (i2c->FFSSTAT.B.FFS == 8u)
        {
            st = tof_protocolStatus(i2c);
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
        txData.word = 0u;
        for (k = 0u; k < 4u; k++)
        {
            uint32 index = (i * 4u) + k;
            if (index < (uint32)count)
            {
                txData.byte[k] = buf[index];
            }
        }
        IfxI2c_writeFifo(i2c, txData.word);
        g_busContext[0] = 3u; /* TX request */
        st = tof_waitRequest(i2c);
        if (st != TOF_OK)
        {
            return st;
        }
        tof_clearRequests(i2c);
    }
    return TOF_OK;
}

/* START + address + ACK, and nothing else. This is the same first packet
 * IfxI2c_I2c_write puts on the wire, and the same reason: with SOPE=0 the module
 * ends the packet in master restart state instead of releasing the bus, so the
 * address gets answered before any payload is committed. An absent module
 * therefore reports as TOF_ERR_NO_ACK rather than as a half-written register. */
static TofStatus tof_probeAddress(Ifx_I2C *i2c, uint8 addrByte)
{
    TofStatus st;

    g_txBuf[0] = addrByte;

    st = tof_txFill(i2c, g_txBuf, 1u);
    if (st != TOF_OK)
    {
        return st;
    }
    st = tof_waitProtocol(i2c, IfxI2c_ProtocolInterruptSource_transmissionEnd);
    if (st != TOF_OK)
    {
        return st;
    }
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

/* One register write: probe, then one repeated-start message of address + index
 * pair + payload, then STOP. At most one message, because a message is capped at
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

    st = tof_probeAddress(i2c, TOF_slaveAddrWrite(addr8));

    if (st == TOF_OK)
    {
        g_txBuf[0] = TOF_slaveAddrWrite(addr8);
        g_txBuf[1] = TOF_wireAddrHiWrite(reg);
        g_txBuf[2] = TOF_wireAddrLo(reg);
        for (i = 0u; i < len; i++)
        {
            g_txBuf[3u + i] = data[i];
        }

        st = tof_txFill(i2c, g_txBuf, msgLen);
    }
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
    st = tof_probeAddress(i2c, TOF_slaveAddrWrite(addr8));
    if (st == TOF_OK)
    {
        g_txBuf[0] = TOF_slaveAddrWrite(addr8);
        g_txBuf[1] = TOF_wireAddrHiWrite(reg);
        g_txBuf[2] = TOF_wireAddrLo(reg);

        st = tof_txFill(i2c, g_txBuf, TOF_txMessageBytes(0u));
    }
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

/* Module + pads, from a known state. Split out because a stalled transaction can
 * only be freed by resetting the module - the same conclusion the QSPI path
 * reached: no service request, no recovery (imu.c:279-282). */
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
    IfxI2c_initSclSdaPin(&IfxI2c0_SCL_P13_1_INOUT, &IfxI2c0_SDA_P13_2_INOUT,
                         IfxPort_PadDriver_cmosAutomotiveSpeed1);

    IfxI2c_run(i2c);

    /* Post-divider value: what the wire actually does, not what was asked for.
     * The bench compares it against a scope reading (doc 23 section 11.4 step 5)
     * - the IMU batch reported the requested clock instead of the measured one,
     * and I2C0 is a different module whose divider that lesson cannot cover. */
    g_actualHz = IfxI2c_getBaudrate(i2c);
}

static void tof_recover(void)
{
    IfxI2c_resetModule(TOF_I2C);
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
    sint32 vals[4];

    g_errLogMs = tof_nowMs();

    vals[0] = (sint32)g_lastErr;
    vals[1] = (sint32)g_stStatus;
    vals[2] = (sint32)g_cfgStep;
    vals[3] = (sint32)g_errCount;
    XCORE_logi("TOFERR berr/st/step/err=", vals, 4u);
    if (g_busDiagValid != FALSE)
    {
        XCORE_logu("TOFBUS phase/rw/reg/len/pirq/err/ris/bs/ffs/scl/sda=", g_busDiag, 11u);
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

    /* Ordinary input, no internal pull device: the module drives INT open-drain
     * and the 47 k pull-up to IOVDD is the datasheet's own requirement (doc 23
     * section 11.1/11.3). Do NOT reach for IfxScuEru on this pin - ERU input
     * channel 0 belongs to the IMU (doc 23 section 11.2). */
    IfxPort_setPinModeInput(TOF_INT_PORT, TOF_INT_PIN, IfxPort_InputMode_noPullDevice);

    /* Unsigned wrap-safe arithmetic: this says "the first task pass probes
     * immediately". The ULD's own identity check is what decides alive. */
    g_reprobeMs = tof_nowMs() - TOF_REPROBE_MS;
    g_logMs     = tof_nowMs();
    g_errLogMs  = tof_nowMs();
}

void TOF_task(void)
{
    uint32 nowMs = tof_nowMs();

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
        sint32 vals[12];
        uint8  nearestStatus = 0u;
        sint16 nearest       = tof_nearestMm(&nearestStatus);

        g_logMs = tof_nowMs();
        vals[0]  = (sint32)g_state;
        vals[1]  = (g_alive != FALSE) ? 1 : 0;
        vals[2]  = (sint32)g_frameCount;
        vals[3]  = (sint32)g_errCount;
        vals[4]  = (sint32)g_initMs;
        vals[5]  = (sint32)g_actualHz;
        vals[6]  = (sint32)TOF_ZONE_COUNT;
        vals[7]  = (sint32)nearest;
        vals[8]  = (sint32)nearestStatus;
        vals[9]  = (sint32)TOF_intLevel();
        vals[10] = (sint32)g_whoAmI[0];     /* expect 240 = 0xF0, 0 until an answer */
        vals[11] = (sint32)g_whoAmI[1];     /* expect 2   = 0x02, 0 until an answer */
        XCORE_logi("TOF", vals, 12u);
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
