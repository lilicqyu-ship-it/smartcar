#include "uart.h"
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

void UART_echoTask(void)
{
    if (IfxAsclin_Asc_getReadCount(&g_asclin) > 0)
    {
        uint8      rx;
        Ifx_SizeT  count = 1;

        if (IfxAsclin_Asc_read(&g_asclin, &rx, &count, TIME_NULL))
        {
            UART_putchar(rx);
        }
    }
}
