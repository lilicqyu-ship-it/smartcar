/*
 * bsp/uart.h - host stand-in for the console UART
 *
 * xcore.c drains its log ring through UART_println; on the host the test
 * binary provides the function and captures the lines, so the drain policy
 * (one line per XCORE_logService call) is asserted instead of trusted. Lives
 * under test/host/stub so "-I test/host/stub" shadows the real bsp/uart.h,
 * which pulls in iLLD headers the host cannot compile.
 */
#ifndef HOST_BSP_UART_H
#define HOST_BSP_UART_H

void UART_println(const char *str);

#endif /* HOST_BSP_UART_H */
