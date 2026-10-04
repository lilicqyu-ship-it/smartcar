/*
 * bsp/uart.h - host stand-in for the console UART
 *
 * xcore.c drains its log ring through UART_printTry, the non-blocking pump
 * into the driver's software TX FIFO; on the host the test binary provides
 * that function on top of a simulated 256 B FIFO plus a "wire" stream, so
 * the pump policy (bounded per call, resumes where it stopped, byte order
 * and CRLF framing intact) is asserted instead of trusted. Lives under
 * test/host/stub so "-I test/host/stub" shadows the real bsp/uart.h, which
 * pulls in iLLD headers the host cannot compile.
 */
#ifndef HOST_BSP_UART_H
#define HOST_BSP_UART_H

#include "Ifx_Types.h"

uint32 UART_printTry(const char *data, uint32 len);

#endif /* HOST_BSP_UART_H */
