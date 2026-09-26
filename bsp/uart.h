#ifndef UART_H
#define UART_H

#include "Ifx_Types.h"

void UART_init(void);
void UART_putchar(uint8 c);
void UART_print(const char *str);
void UART_println(const char *str);
void UART_echoTask(void);

#endif