#include "uart.h"
#include "IfxAsclin_reg.h"
#include "IfxScuWdt.h"
#include "IfxPort.h"

#define UART_BAUDRATE  115200

void UART_init(void)
{
    uint16 psw;

    /* ---- 1. Reset and enable ASCLIN0 module ---- */
    psw = IfxScuWdt_getCpuWatchdogPassword();
    IfxScuWdt_clearCpuEndinit(psw);

    /* Reset module: set both reset bits, wait, then clear */
    ASCLIN0_KRST0.B.RST = 1;
    ASCLIN0_KRST1.B.RST = 1;
    while (0 == ASCLIN0_KRST0.B.RSTSTAT) {}
    ASCLIN0_KRSTCLR.B.CLR = 1;

    /* Enable module */
    ASCLIN0_CLC.B.DISR = 0;

    IfxScuWdt_setCpuEndinit(psw);

    /* ---- 2. Select kernel clock source ---- */
    ASCLIN0_CSR.B.CLKSEL = 1;  /* fCLK = kernel clock (fSPB) */

    /* Wait for clock to be active */
    while (ASCLIN0_CSR.B.CON == 0) {}

    /* ---- 3. Configure baud rate (115200) ---- */
    /* Prescaler = 1, so fPD = fSPB / 1 = 100MHz */
    ASCLIN0_BITCON.B.PRESCALER = 1 - 1;  /* register value = prescaler - 1 */
    /* Oversampling factor = 4 (register value 3) */
    ASCLIN0_BITCON.B.OVERSAMPLING = 3;
    /* Sample point position = 3 */
    ASCLIN0_BITCON.B.SAMPLEPOINT = 3;
    /* One sample per bit (median filter disabled) */
    ASCLIN0_BITCON.B.SM = 0;

    /* fOvs = baudrate * (OVERSAMPLING + 1) = 115200 * 4 = 460800
     * fPD = fSPB / (PRESCALER + 1) = 100MHz / 1 = 100MHz
     * fShift = fOvs / (OVERSAMPLING + 1) = fPD * NUM/DEN / 4 = 25MHz * NUM/DEN
     * NUMERATOR / DENOMINATOR = 115200 / 25MHz = 0.004608
     * Choose: NUMERATOR = 1, DENOMINATOR = 217
     * Actual baud = 25MHz * 1/217 = 115207 (0.006% error)
     */
    ASCLIN0_BRG.B.NUMERATOR   = 1;
    ASCLIN0_BRG.B.DENOMINATOR = 217;

    /* ---- 4. Configure frame format: 8N1, LSB first ---- */
    ASCLIN0_FRAMECON.B.MODE = 1;  /* ASC mode */
    ASCLIN0_FRAMECON.B.STOP = 1;  /* 1 stop bit */
    ASCLIN0_FRAMECON.B.PEN  = 0;  /* no parity */
    ASCLIN0_FRAMECON.B.ODD  = 0;  /* even parity (not used) */
    ASCLIN0_FRAMECON.B.MSB  = 0;  /* LSB first */
    ASCLIN0_FRAMECON.B.IDLE = 0;  /* no idle delay */

    /* 8-bit data length */
    ASCLIN0_DATCON.B.DATLEN = 8 - 1;

    /* ---- 5. Configure FIFO ---- */
    /* Enable TX FIFO outlet (transmit) */
    ASCLIN0_TXFIFOCON.B.ENO = 1;
    /* Enable RX FIFO inlet (receive) */
    ASCLIN0_RXFIFOCON.B.ENI = 1;

    /* ---- 6. Configure pins (P14.0 = TX, P14.1 = RX) ---- */
    /* P14.0: ALT2 output (push-pull) for ASCLIN0 TX */
    IfxPort_setPinMode(&MODULE_P14, 0, IfxPort_Mode_outputPushPullAlt2);
    /* P14.1: input with pull-up for ASCLIN0 RX */
    IfxPort_setPinMode(&MODULE_P14, 1, IfxPort_Mode_inputPullUp);
    /* Set pad driver strength */
    IfxPort_setPinPadDriver(&MODULE_P14, 0, IfxPort_PadDriver_cmosAutomotiveSpeed1);
    IfxPort_setPinPadDriver(&MODULE_P14, 1, IfxPort_PadDriver_cmosAutomotiveSpeed1);

    /* Select RX input pin: P14.1 = ALT1 (Ifx_RxSel_a) */
    ASCLIN0_IOCR.B.ALTI = 0;

    /* ---- 7. Clear flags and flush FIFOs ---- */
    /* Disable all flags */
    ASCLIN0_FLAGSENABLE.U = 0;
    /* Clear all flags */
    ASCLIN0_FLAGSCLEAR.U = 0xFFFFFFFF;

    /* Flush FIFOs */
    ASCLIN0_TXFIFOCON.B.FLUSH = 1;
    ASCLIN0_RXFIFOCON.B.FLUSH = 1;
}

void UART_putchar(uint8 c)
{
    /* Wait until TX FIFO has space (16 entries deep) */
    while (ASCLIN0_TXFIFOCON.B.FILL >= 16) {}

    /* Write data to TX FIFO */
    ASCLIN0_TXDATA.U = c;
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