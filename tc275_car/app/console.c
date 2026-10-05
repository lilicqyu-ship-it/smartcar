/* app/console.c — ASCLIN0 line-command console (CPU0).
 *
 * Line handling is bounded and hardware-free (host-tested in
 * test/host/test_console.c): bytes accumulate in a 24-char buffer, CR/LF
 * dispatches, and an overlong line is dropped whole with one "ERR: line"
 * reply instead of being truncated into a wrong command. Dispatch touches
 * only the xcore bench-log switch and the log ring, both lock-guarded, so
 * this can run from a priority-1 task next to the robot task. */
#include "app/console.h"
#include "bsp/uart.h"
#include "mw/xcore/xcore.h"
#include <string.h>

#define CONSOLE_LINE_MAX 24u    /* longest command is "BENCH off" = 9 chars */

static struct
{
    char    buf[CONSOLE_LINE_MAX + 1u];
    uint8   len;
    boolean overflow;
} s_con;

static void console_dispatch(const char *line)
{
    if (strcmp(line, "BENCH on") == 0)
    {
        XCORE_benchLogSet(TRUE);
        XCORE_logln("BENCH=on");
    }
    else if (strcmp(line, "BENCH off") == 0)
    {
        XCORE_benchLogSet(FALSE);
        XCORE_logln("BENCH=off");
    }
    else if (strcmp(line, "BENCH?") == 0)
    {
        XCORE_logln(XCORE_benchLogActive() ? "BENCH=on" : "BENCH=off");
    }
    else if (strcmp(line, "HELP") == 0)
    {
        XCORE_logln("HELP: BENCH on|off|?");
    }
    else
    {
        XCORE_logln("ERR: HELP");
    }
}

static void console_feed(uint8 c)
{
    if ((c == '\r') || (c == '\n'))
    {
        if (s_con.overflow)
        {
            XCORE_logln("ERR: line");
        }
        else if (s_con.len > 0u)
        {
            s_con.buf[s_con.len] = '\0';
            console_dispatch(s_con.buf);
        }
        s_con.len      = 0u;
        s_con.overflow = FALSE;
        return;
    }

    if (s_con.len >= CONSOLE_LINE_MAX)
    {
        s_con.overflow = TRUE;          /* keep eating until the terminator */
        return;
    }
    s_con.buf[s_con.len++] = (char)c;
}

void CONSOLE_task(void)
{
    uint8 c;

    while (UART_readByte(&c))
    {
        console_feed(c);
    }
}
