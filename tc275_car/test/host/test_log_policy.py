#!/usr/bin/env python3
"""Exercise production diagnostic gates, including disconnected slave and jitter."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
link = (root / "com/link.c").read_text(encoding="utf-8")
link = link[link.index("void LINK_diagPrint(void)"):]
motor = (root / "rt/motor_algo.c").read_text(encoding="utf-8")
start = motor.index("#define MOTOR_ALGO_BENCH_PERIOD_MS")
end = motor.index("\n}", start) + 2
motor = motor[start:end]
harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mw/xcore/xcore.h"
#define LINK_READY 1u
#define LINK_LOST 2u
#define LINK_READY_MAGIC 0x5F534601u
#define LINK_REG_READY 0u
static uint32 now, ready;
static int lines;
static char uart[64][260];
void UART_println(const char *s) { if(lines<64) { strcpy(uart[lines++],s); } }
static uint32 STIME_nowMs(void) { return now; }
static boolean SPIHAL_irqAsserted(void) { return FALSE; }
static uint32 g_reg;
static uint32 link_reg(uint32 reg, uint32 address) { (void)reg; (void)address; return ready; }
static struct {
    uint32 state,clockHz,slaveErrStat,txPending,rxRoom,sinceAliveMs;
    struct { uint32 timeouts,hwErrors,transactions; } spi;
    struct { uint32 spiErrors,crcErrors,seqErrors,txQueueFull,cmdRejectedQueue,
                    wrSegments,rdSegments,txFrames; } stats;
} g_health;
static struct { sint16 target; } g_left,g_right;
static boolean g_servoLogAsked;
static void drain(void) { int i; for(i=0;i<10;i++) XCORE_logService(); }
/* xcore.c drains the log ring through the non-blocking byte pump
 * (UART_printTry) since the UART FIFO change; reassemble lines on '\n' so
 * the line-counting assertions below keep reading like a terminal. */
static char curLine[300]; static int curLen;
uint32 UART_printTry(const char *d, uint32 n) {
    uint32 i;
    for(i=0;i<n;i++){
        char c=d[i];
        if(c=='\n'){ if(lines<64){ curLine[curLen]=0; strcpy(uart[lines++],curLine); } curLen=0; }
        else if(c!='\r' && curLen<299){ curLine[curLen++]=c; }
    }
    return n;
}
'''
checks = r'''
int main(void) {
    XcoreEncoder enc;
    int baseline;
    XCORE_init();
    ready=0xFFFFFFFFu; g_health.slaveErrStat=0xFFFFFFFFu;
    g_health.clockHz=1000000u;
    LINK_diagPrint(); drain(); baseline=lines;
    assert(baseline==4);
    assert(strstr(uart[0],"registers_valid=0") && strstr(uart[1],"data=unknown"));
    now=500u; g_health.spi.transactions++; LINK_diagPrint(); drain(); assert(lines==baseline);
    now=29999u; LINK_diagPrint(); drain(); assert(lines==baseline);
    now=30000u; LINK_diagPrint(); drain(); assert(lines==baseline+4); baseline=lines;
    now=30001u; ready=LINK_READY_MAGIC; g_health.state=LINK_READY;
    g_health.slaveErrStat=0u; LINK_diagPrint(); drain(); assert(lines==baseline+4); baseline=lines;
    now=30002u; g_health.spi.timeouts=1u; LINK_diagPrint(); drain(); assert(lines==baseline+4); baseline=lines;
    now=30003u; g_health.spi.timeouts=2u; LINK_diagPrint(); drain(); assert(lines==baseline);
    now=35002u; LINK_diagPrint(); drain(); assert(lines==baseline+4); baseline=lines;
    now=65002u; LINK_diagPrint(); drain(); assert(lines==baseline+4);
    XCORE_init(); lines=0; curLen=0; memset(&enc,0,sizeof(enc));
    now=100u; enc.pctLeft=8; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==1);
    now=101u; enc.pctLeft=0; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f);
    now=102u; enc.pctLeft=8; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==1);
    now=5099u; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==1);
    now=5100u; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==2);
    now=10100u; enc.pctLeft=0; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==2);
    now=10101u; enc.pctLeft=8; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==3);
    /* BENCH on: 10 ms SRVB stream, immune to the idle gate (a continuous
     * timebase is the point of the MATLAB live plot), integral x10 encoded
     * into the line. */
    XCORE_init(); lines=0; curLen=0; memset(&enc,0,sizeof(enc));
    XCORE_benchLogSet(TRUE);
    now=20000u; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==1);
    /* XCORE_logi wire format: label, space, space-separated signed values. */
    assert(strstr(uart[0],"SRVB ")==uart[0]);
    { int a,b,c,d,e,f,g,h,i;
      assert(sscanf(uart[0],"SRVB %d %d %d %d %d %d %d %d %d",
                    &a,&b,&c,&d,&e,&f,&g,&h,&i)==9);
      assert(a==20000); }
    now=20005u; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==1);
    now=20010u; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==2);
    now=20020u; MOTOR_ALGO_diag(&enc,120,80,12.5f,-3.0f); drain(); assert(lines==3);
    assert(strstr(uart[2]," 125 ")!=NULL);   /* 12.5 * 10 */
    assert(strstr(uart[2],"-30")!=NULL);     /* -3.0 * 10 */
    /* BENCH off: stream stops, the 5 s [SERVO] policy resumes exactly. */
    XCORE_benchLogSet(FALSE);
    now=20025u; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==3);
    now=25025u; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==3);
    now=25026u; enc.pctLeft=8; MOTOR_ALGO_diag(&enc,0,0,0.0f,0.0f); drain(); assert(lines==4);
    assert(strstr(uart[3],"[SERVO]")==uart[3]);
    puts("PASS: log policy disconnected sentinel, heartbeat, transitions, repeated faults, servo jitter, bench stream");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="log-policy-") as directory:
    work=Path(directory)
    source=work/"policy.c"
    source.write_text(harness+"\n"+link+"\n"+motor+"\n"+checks, encoding="utf-8")
    binary=work/"policy"
    subprocess.run([os.environ.get("CC","cc"),"-std=c99","-Wall","-Wextra","-Werror","-O2",
        "-I",str(root/"test/host/stub"),"-I",str(root),str(source),
        str(root/"mw/xcore/xcore.c"),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
