#include "bsp/uart.h"
#include "IfxAsclin_Asc.h"
#include "IfxAsclin_PinMap.h"
#include "IfxPort.h"

#define UART_BAUDRATE  115200

#define ISR_PRIORITY_ASCLIN_TX  8
#define ISR_PRIORITY_ASCLIN_RX  4
#define ISR_PRIORITY_ASCLIN_ER  12

#define ASC_TX_BUFFER_SIZE      256
#define ASC_RX_BUFFER_SIZE      256

static IfxAsclin_Asc g_asclin;

static uint8 g_uartTxBuffer[ASC_TX_BUFFER_SIZE + sizeof(Ifx_Fifo) + 8];
static uint8 g_uartRxBuffer[ASC_RX_BUFFER_SIZE + sizeof(Ifx_Fifo) + 8];

IFX_INTERRUPT(asc0TxISR, 0, ISR_PRIORITY_ASCLIN_TX);

void asc0TxISR(void)
{
    IfxAsclin_Asc_isrTransmit(&g_asclin);
}

IFX_INTERRUPT(asc0RxISR, 0, ISR_PRIORITY_ASCLIN_RX);

void asc0RxISR(void)
{
    IfxAsclin_Asc_isrReceive(&g_asclin);
}

IFX_INTERRUPT(asc0ErrISR, 0, ISR_PRIORITY_ASCLIN_ER);

void asc0ErrISR(void)
{
    IfxAsclin_Asc_isrError(&g_asclin);
}

void UART_init(void)
{
    IfxAsclin_Asc_Config ascConf;

    IfxAsclin_Asc_initModuleConfig(&ascConf, &MODULE_ASCLIN0);

    ascConf.baudrate.baudrate       = UART_BAUDRATE;
    ascConf.baudrate.oversampling   = IfxAsclin_OversamplingFactor_16;
    ascConf.bitTiming.medianFilter  = IfxAsclin_SamplesPerBit_three;
    ascConf.bitTiming.samplePointPosition = IfxAsclin_SamplePointPosition_8;

    ascConf.interrupt.txPriority    = ISR_PRIORITY_ASCLIN_TX;
    ascConf.interrupt.rxPriority    = ISR_PRIORITY_ASCLIN_RX;
    ascConf.interrupt.erPriority    = ISR_PRIORITY_ASCLIN_ER;
    ascConf.interrupt.typeOfService = IfxSrc_Tos_cpu0;

    const IfxAsclin_Asc_Pins pins = {
            NULL_PTR,                       IfxPort_InputMode_pullUp,
            &IfxAsclin0_RXA_P14_1_IN,       IfxPort_InputMode_pullUp,
            NULL_PTR,                       IfxPort_OutputMode_pushPull,
            &IfxAsclin0_TX_P14_0_OUT,       IfxPort_OutputMode_pushPull,
            IfxPort_PadDriver_cmosAutomotiveSpeed1
    };
    ascConf.pins = &pins;

    ascConf.txBuffer     = g_uartTxBuffer;
    ascConf.txBufferSize = ASC_TX_BUFFER_SIZE;
    ascConf.rxBuffer     = g_uartRxBuffer;
    ascConf.rxBufferSize = ASC_RX_BUFFER_SIZE;

    IfxAsclin_Asc_initModule(&g_asclin, &ascConf);
}

void UART_putchar(uint8 c)
{
    IfxAsclin_Asc_blockingWrite(&g_asclin, c);
}

void UART_print(const char *str)
{
    while (*str != '\0')
    {
        if (*str == '\n')
        {
            UART_putchar('\r');
        }
        UART_putchar(*str);
        str++;
    }
}

void UART_println(const char *str)
{
    UART_print(str);
    UART_putchar('\r');
    UART_putchar('\n');
}

/* Non-blocking console write for the log pump: queues whole source bytes
 * into the driver's software TX FIFO, from where the TX ISR shifts them out
 * at line rate in the background. Returns the number of SOURCE bytes
 * accepted; a byte the FIFO could not take is left with the caller. '\n'
 * expands to CRLF, and only when both bytes fit, so a newline is never
 * consumed with half of it queued - the byte order on the wire stays exactly
 * the ring order across partial pumps. */
uint32 UART_printTry(const char *data, uint32 len)
{
    uint32 accepted = 0;

    while (accepted < len)
    {
        char   c    = data[accepted];
        uint32 need = (c == '\n') ? 2u : 1u;    /* CRLF takes two slots */

        if ((uint32)IfxAsclin_Asc_getWriteCount(&g_asclin) < need)
        {
            break;                              /* software TX FIFO full */
        }

        if (c == '\n')
        {
            uint8     crlf[2] = { (uint8)'\r', (uint8)'\n' };
            Ifx_SizeT count   = 2;

            if (IfxAsclin_Asc_write(&g_asclin, crlf, &count, TIME_NULL) == FALSE)
            {
                break;
            }
        }
        else
        {
            Ifx_SizeT count = 1;

            if (IfxAsclin_Asc_write(&g_asclin, &c, &count, TIME_NULL) == FALSE)
            {
                break;
            }
        }
        accepted++;
    }

    return accepted;
}

/* One RX byte for the console parser (app/console.c): FALSE when the driver's
 * software RX FIFO is dry. Non-blocking, so the console task can poll. */
boolean UART_readByte(uint8 *c)
{
    Ifx_SizeT count = 1;

    if ((c == NULL_PTR) || (IfxAsclin_Asc_getReadCount(&g_asclin) <= 0))
    {
        return FALSE;
    }
    return IfxAsclin_Asc_read(&g_asclin, c, &count, TIME_NULL);
}

/* Polled TX pump for the fatal-error path (Cpu0 stack-overflow hook),
 * called with interrupts off: nothing else moves the software FIFO into
 * the ASCLIN, so this performs the very transfer the TX ISR would do.
 * The bound is iterations, not bytes -- with IRQs off the shift register
 * drains at line rate (one byte ~ 87 us at 115200), so each byte needs
 * thousands of spin iterations before the hardware has room again. The
 * figure below covers a full 256-byte FIFO (~22 ms) at 200 MHz; it exists
 * only to keep a dead peripheral from hanging the fatal path, where the
 * CPU watchdog is the real exit. */
#define UART_FLUSH_GUARD 200000u

void UART_flushPolling(void)
{
    uint32 guard = UART_FLUSH_GUARD;

    while ((guard > 0u) && (Ifx_Fifo_isEmpty(g_asclin.tx) == FALSE))
    {
        IfxAsclin_Asc_isrTransmit(&g_asclin);
        guard--;
    }
}
