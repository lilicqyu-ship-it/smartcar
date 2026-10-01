/*
 * spi_hal_pins.h - TC275 QSPI3 master half of the production LINK (CPU2 owner)
 *
 * Wire truth source: doc/20-design/22-link-spi-design.md SS3 (pins) and SS4
 * (transaction model). This module owns the pin muxing, the clock ladder and
 * ONE raw half-duplex transaction; the framing and the pump live in com/link.c.
 *
 * Pin map (22 SS3.1, all symbols taken from Libraries/.../_PinMap/IfxQspi_PinMap.h):
 *   P33.11  QSPI3_SCLK -> ESP32-C6 GPIO19
 *   P33.12  QSPI3_MTSR -> ESP32-C6 GPIO18   (MOSI, master transmit)
 *   P33.13  QSPI3_MRSTD<- ESP32-C6 GPIO20   (MISO, master receive)
 *   P23.4   QSPI3_SLSO5-> ESP32-C6 GPIO23   (CS, active low)
 *   P23.0   general purpose input <- ESP32-C6 GPIO21 (IRQ, open drain; the
 *           boards are jumper wires only, so the pull-up is this pin's own)
 * P33.0..7 stay free for the encoders and P23.4 is the only usable chip select
 * (SDD SS18 C7).
 *
 * The preamble problem (22 SS2 E8 / R7): AURIX QSPI has no command or address
 * phase, so SPIHAL_transaction() puts CMD, ADDR and DUMMY into the DATA phase as
 * three ordinary 8-bit words at the head of one exchange(). CS is held low for
 * the whole exchange (hardware auto-CS with channelBasedCs disabled writes a
 * "begin stream" BACON before the first word and an "end stream" BACON with
 * LAST=1 for the last one), which is what Espressif's slave expects to see as
 * CMD(8)+ADDR(8)+DUMMY(8)+data inside one chip select.
 *
 * Interrupts: declared in vector table 0 and routed to CPU2 through the SRC TOS
 * bit (SDD SS18 C1/C2). Priorities 6/9/10 are the free slots next to ASCLIN1's
 * 5/7/13 on this core.
 */
#ifndef SPI_HAL_PINS_H
#define SPI_HAL_PINS_H

#include "Ifx_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Espressif slave-HD command bytes, 1-line mode -------------------------
 * Value = base command | line-mode modifier; the modifier is 0x00 for
 * CMD/ADDR/DATA all on one line, which is all the TC275 QSPI can do.
 * Evidence: esp-idf components/hal/include/hal/spi_types.h (SPI_CMD_HD_*),
 * components/hal/esp32c6/include/hal/spi_ll.h (spi_ll_get_slave_hd_command). */
#define SPIHD_CMD_WRBUF      0x01u  /* master writes the 64 B shared register file */
#define SPIHD_CMD_RDBUF      0x02u  /* master reads the 64 B shared register file  */
#define SPIHD_CMD_WRDMA      0x03u  /* master writes one segment into slave RX     */
#define SPIHD_CMD_RDDMA      0x04u  /* master reads one segment out of slave TX    */
#define SPIHD_CMD_SEG_END    0x05u
#define SPIHD_CMD_WR_END     0x07u  /* closes a WRDMA burst                        */
#define SPIHD_CMD_INT0       0x08u  /* closes a RDDMA burst                        */

/* Preamble length in data bytes: CMD, ADDR, DUMMY (dummy bits are fixed at 8,
 * spi_ll_get_slave_hd_dummy_bits()). The slave echoes 3 junk bytes back on a
 * read, so RX payloads start at offset 3. */
#define SPIHD_PREAMBLE_LEN   3u

/* Largest segment carried in one transaction: one padded max SF frame. */
#define SPIHAL_MAX_DATA      260u
#define SPIHAL_MAX_XFER      (SPIHD_PREAMBLE_LEN + SPIHAL_MAX_DATA)

/* ---- clock ladder (22 SS3.2 / SS8 G5) ------------------------------------- */
typedef enum
{
    SPIHAL_CLK_1M = 0,
    SPIHAL_CLK_2M,
    SPIHAL_CLK_5M,      /* production baseline */
    SPIHAL_CLK_10M,
    SPIHAL_CLK_20M,     /* exploration only, not committed */
    SPIHAL_CLK_COUNT
} SpiHal_ClockTier;

typedef enum
{
    SPIHAL_OK = 0,
    SPIHAL_ERR_PARAM,     /* null pointer or length beyond the buffer */
    SPIHAL_ERR_BUSY,      /* a transaction is still in flight */
    SPIHAL_ERR_TIMEOUT,   /* QSPI did not complete inside the timeout */
    SPIHAL_ERR_HW         /* QSPI error interrupt latched */
} SpiHal_Status;

typedef struct
{
    uint32 transactions;
    uint32 timeouts;      /* the QSPI stalled; the driver re-armed itself */
    uint32 hwErrors;      /* QSPI error interrupt latched */
    uint32 busyRejects;   /* exchange() refused to start: a programming error
                           * here, since CPU2 is the only owner */
} SpiHal_Stats;

/* Bring up QSPI3 on CPU2 and mux P23.0 as the IRQ input.
 * Must be called after STIME_init() and before the first transaction. */
void SPIHAL_init(SpiHal_ClockTier tier);

/* Runtime clock switch for the G5 ladder; takes effect on the next transaction
 * and is only safe when no transaction is in flight. */
SpiHal_Status SPIHAL_setClock(SpiHal_ClockTier tier);
SpiHal_ClockTier SPIHAL_getClock(void);
uint32 SPIHAL_clockHz(SpiHal_ClockTier tier);

/* Baudrate the channel is actually running at after divider quantisation, so
 * the G5 ladder can be reported from measured values instead of requested ones. */
uint32 SPIHAL_actualClockHz(void);

/* One chip-select assertion = [cmd][addr][dummy] + data.
 *   read  = TRUE : clock in dataLen bytes, caller discards txData contents
 *   read  = FALSE: clock out txData
 * dataLen must be a multiple of 4 (the slave's RX buffers are word aligned,
 * 22 SS5.1 padding rule). Blocks until the QSPI finished or the transaction
 * timed out; the pump calls it from the CPU2 superloop, never from an ISR.
 * rxData receives the payload only (the 3 preamble bytes are dropped). */
SpiHal_Status SPIHAL_transaction(uint8 cmd, uint8 addr,
                                const uint8 *txData, uint8 *rxData,
                                uint16 dataLen, boolean read);

/* TRUE while the previous transaction has not completed yet. */
boolean SPIHAL_isBusy(void);

/* Level of P23.0. TRUE = the slave is asserting its IRQ line, i.e. it has
 * something queued for the master to pull. Polled, not interrupt driven:
 * P23.x is not an IOM monitor input on this package, so no edge interrupt
 * exists for this pin (22 SS4.1 write-back). */
boolean SPIHAL_irqAsserted(void);

void SPIHAL_getStats(SpiHal_Stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* SPI_HAL_PINS_H */
