/* ASCLIN0 one-line command console (CPU0), the bench entry point for the
 * MATLAB live plot (matlab_motor_model/live_serial_plot.m). Commands are
 * exact-match lines terminated by CR, LF or CRLF:
 *
 *   BENCH on    100 Hz SRVB speed-loop stream on the console UART
 *   BENCH off   back to the 5 s [SERVO] line
 *   BENCH?      current stream state ("BENCH=on" / "BENCH=off")
 *   HELP        one-line command list
 *
 * Replies go through the log ring (XCORE_logln), so they interleave with
 * the rest of the console output. Unknown or broken lines answer "ERR:",
 * which also makes typing any command a loopback check for the USB-UART
 * path (the former echo task's job). Pure line handling here - the only
 * hardware touch is polling UART_readByte(). */
#ifndef APP_CONSOLE_H
#define APP_CONSOLE_H

/* Drain the RX FIFO and dispatch every complete line. Call periodically
 * from a low-priority CPU0 task; non-blocking. */
void CONSOLE_task(void);

#endif /* APP_CONSOLE_H */
