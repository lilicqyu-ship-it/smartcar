/*
 * ASCLIN0 console end-to-end on the host: scripted RX bytes through a
 * UART_readByte stub drive the real CONSOLE_task; replies are reassembled
 * from the log ring via the UART_printTry pump (same trick as
 * test_log_policy.py) and the BENCH switch is asserted through the real
 * xcore flag that CPU1's MOTOR_ALGO_diag would read.
 *
 * gcc -std=c99 -Wall -Wextra -Werror -O2 -I test/host/stub -I . \
 *     test/host/test_console.c app/console.c mw/xcore/xcore.c
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app/console.h"
#include "bsp/uart.h"
#include "mw/xcore/xcore.h"

/* ---- scripted RX wire: UART_readByte hands out one byte per poll --------- */
static const char *s_script;
static int         s_pos;

boolean UART_readByte(uint8 *c)
{
    if ((s_script != NULL) && (s_script[s_pos] != '\0'))
    {
        *c = (uint8)s_script[s_pos++];
        return TRUE;
    }
    return FALSE;
}

/* ---- reply capture: log ring -> UART_printTry pump -> whole lines -------- */
#define MAX_LINES 32
static char s_lines[MAX_LINES][64];
static int  s_count;
static char s_cur[64];
static int  s_curLen;

uint32 UART_printTry(const char *d, uint32 n)
{
    uint32 i;

    for (i = 0; i < n; i++)
    {
        char c = d[i];

        if (c == '\n')
        {
            if (s_count < MAX_LINES)
            {
                s_cur[s_curLen] = '\0';
                strcpy(s_lines[s_count++], s_cur);
            }
            s_curLen = 0;
        }
        else if ((c != '\r') && (s_curLen < 62))
        {
            s_cur[s_curLen++] = c;
        }
    }
    return n;
}

/* Deliver one script and drain the log pump. */
static void feed(const char *script)
{
    int i;

    s_script = script;
    s_pos    = 0;
    CONSOLE_task();
    for (i = 0; i < 10; i++)
    {
        XCORE_logService();
    }
}

int main(void)
{
    XCORE_init();
    assert(XCORE_benchLogActive() == FALSE);

    /* on / off / query, CRLF and bare-LF terminators both accepted */
    feed("BENCH on\r\n");
    assert(XCORE_benchLogActive() == TRUE);
    assert((s_count == 1) && (strcmp(s_lines[0], "BENCH=on") == 0));

    feed("BENCH off\n");
    assert(XCORE_benchLogActive() == FALSE);
    assert((s_count == 2) && (strcmp(s_lines[1], "BENCH=off") == 0));

    feed("BENCH?\n");                /* query still needs a terminator     */
    assert((s_count == 3) && (strcmp(s_lines[2], "BENCH=off") == 0));

    feed("BENCH on\r");              /* CR alone dispatches */
    feed("\n");                      /* LF of the pair: empty line, silent  */
    assert(XCORE_benchLogActive() == TRUE);
    assert(s_count == 4);

    /* HELP and unknown input */
    feed("HELP\n");
    assert((s_count == 5) && (strcmp(s_lines[4], "HELP: BENCH on|off|?") == 0));

    feed("bench on\n");              /* exact match only */
    assert((s_count == 6) && (strcmp(s_lines[5], "ERR: HELP") == 0));
    assert(XCORE_benchLogActive() == TRUE);

    feed("");                        /* dry FIFO: no dispatch, no reply */
    assert(s_count == 6);

    /* Overlong line: dropped whole with one ERR, parser stays in sync.
     * The long line carries its own terminator - bytes after it belong to
     * the next command. */
    {
        char long_line[64];
        memset(long_line, 'A', 40);
        long_line[40] = '\n';
        long_line[41] = '\0';
        feed(long_line);
        feed("BENCH off\n");
        assert(XCORE_benchLogActive() == FALSE);
        assert((s_count == 8) && (strcmp(s_lines[6], "ERR: line") == 0) &&
               (strcmp(s_lines[7], "BENCH=off") == 0));
    }

    /* Bytes split across CONSOLE_task calls still form one command */
    feed("BEN");
    assert(s_count == 8);            /* nothing complete yet               */
    feed("CH on\r\n");
    assert(XCORE_benchLogActive() == TRUE);
    assert((s_count == 9) && (strcmp(s_lines[8], "BENCH=on") == 0));

    puts("PASS: console on/off/query, terminators, HELP/ERR, overlong drop and resync, split delivery");
    return 0;
}
