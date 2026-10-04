"""Exercise production CPU2 dispatch/queue rejection, using mocked boundaries.
No transport hardware is emulated; functions below are extracted verbatim.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "com/link.c").read_text(encoding="utf-8")
functions = source[source.index("static sint8 link_narrowPercent"):source.index("static void link_feedSegment")]
prefix = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "mw/xcore/xcore.h"
#include "mw/sf/sf_frame.h"
#include "mw/diag/diag_wire.h"
#define LINK_DRIVE_V_FULL 600
#define LINK_DRIVE_W_FULL 300
#define LINK_DRV_PAYLOAD_LEN 5
#define LINK_OP_DRIVE 0x50
#define LINK_CMD_DATA_MAX 16
#define LINK_CMDQ_FULL_LOG_MS 5000
#define XCORE_LOG_FIELDS_TEST(...) ((void)0)
#undef XCORE_LOG_FIELDS
#define XCORE_LOG_FIELDS(...) ((void)suppressed)
static struct { unsigned cmdOversize,cmdRejectedQueue,cmdForwarded,unhandledType,cmdUnsupportedCid,cmdBadLen; } g_stats;
static unsigned normal,latest,diagnostic,estops,replyCount,room=1;
static XcoreCmdMsg last;
static uint8 response[32];
static uint32 STIME_nowMs(void) { return 123; }
boolean XCORE_cmdPush(const XcoreCmdMsg *m) { normal++; last=*m; return (boolean)room; }
boolean XCORE_cmdPushLatest(const XcoreCmdMsg *m) { latest++; last=*m; return (boolean)room; }
boolean XCORE_diagCmdPush(const XcoreCmdMsg *m) { diagnostic++; last=*m; return (boolean)room; }
void XCORE_estopRequest(void) { estops++; }
static boolean LINK_send(uint8 type,uint8 cid,const uint8 *p,uint8 len) {
 assert(type==5 && cid==0x28 && len<=32); memcpy(response,p,len); replyCount++; return TRUE;
}
static void OTARX_frame(uint8 t,uint8 c,const uint8 *p,uint16 len) { (void)t;(void)c;(void)p;(void)len; }
'''
suffix = r'''
int main(void) {
 SF_Frame f; uint8 payload[248]={0}; memset(&f,0,sizeof(f)); f.payload=payload; f.type=1; f.cid=3; f.len=17;
 payload[0]=0x53; payload[1]=0x60; payload[2]=1;
 DT_put32(payload+5,0x12345678); DT_put16(payload+9,7);
 link_dispatch(&f); assert(diagnostic==1 && !normal && !latest && last.len==16);
 room=0; link_dispatch(&f); assert(response[2]==DT_BUSY && replyCount==1 && !normal && !latest);
 assert(DT_u32(response+4)==0x12345678 && DT_u16(response+8)==7);
 f.len=18; link_dispatch(&f); assert(response[2]==DT_BAD_LENGTH && diagnostic==2);
 f.len=17; f.flags=1; link_dispatch(&f); assert(response[2]==DT_BAD_ARGUMENT);
 f.flags=0; f.cid=4; link_dispatch(&f); assert(response[2]==DT_BAD_ARGUMENT);
 f.cid=1; f.len=5; payload[0]=0x10; DT_put16(payload+1,20); DT_put16(payload+3,(uint16)-30);
 room=1; link_dispatch(&f); assert(latest==1 && last.data[0]==20 && (sint8)last.data[1]==-30);
 payload[0]=0x32; room=0; link_dispatch(&f); assert(estops==1 && normal==1);
 puts("production diagnostic dispatch: isolation, correlated rejections, driver and estop paths passed");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="diag-link-test-") as folder:
    src = Path(folder) / "model.c"
    exe = Path(folder) / ("model.exe" if os.name == "nt" else "model")
    src.write_text(prefix + functions + suffix, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I", str(ROOT / "test/host/stub"), "-I", str(ROOT), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
