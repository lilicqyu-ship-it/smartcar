"""Versioned sensor event -> C6 JSON, using the production formatter verbatim."""
import os
import re
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
source=(ROOT/"esp32c6_car/components/c6_bridge/bridge.c").read_text(encoding="utf-8")
func="static void bridge_emit_fusion"+source.split("static void bridge_emit_fusion",1)[1].split("static void pump_link_frame",1)[0]
cpu2=(ROOT/"tc275_car/Cpu2_Main.c").read_text(encoding="utf-8")
producer="static void produce(void) {\n"+re.search(r"if \(LINK_isUp\(\)\) \{.*?\n            \}",cpu2,re.S).group(0)+"\n}\n"
model=r"""
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "app/fusion.h"
typedef uint32_t uint32; typedef uint16_t uint16; typedef uint8_t uint8;
#define SF_TYPE_EVT 5u
static unsigned now, sends, up=1;
static FusionOutput snapshot;
static uint8_t sent[FUSION_WIRE_LEN];
static unsigned STIME_nowMs(void){return now;}
static unsigned LINK_isUp(void){return up;}
static void XCORE_fusionRead(FusionOutput *p){*p=snapshot;}
static unsigned LINK_send(unsigned type,unsigned cid,const uint8_t *p,unsigned n){
 assert(type==SF_TYPE_EVT && cid==FUSION_EVT_CID && n==FUSION_WIRE_LEN);
 memcpy(sent,p,n);sends++;return 1;
}
static char got[256];
static uint16_t proto_get_u16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static void http_broadcast_ctl(const char *s){snprintf(got,sizeof(got),"%s",s);}
"""+func+producer+r"""
int main(void){
 FusionOutput o={0};uint8_t p[FUSION_WIRE_LEN];
 o.flags=71;o.reason=2;o.nearestMm=100;o.speedMmS=-123;o.rollCdeg=-2000;o.tofAgeMs=80;o.validZones=16;o.brake=1;
 FUSION_encode(&o,p);bridge_emit_fusion(p,27);assert(!got[0]);
 p[0]=2;bridge_emit_fusion(p,28);assert(!got[0]);
 p[0]=1;bridge_emit_fusion(p,28);
 puts(got);assert(strstr(got,"\"speed\":-123") && strstr(got,"\"roll\":-2000"));
 assert(strstr(got,"\"flags\":71") && strstr(got,"\"brake\":1"));
 snapshot=o;snapshot.stampMs=10;snapshot.flags=7;snapshot.capMmS=700;
 now=200;produce();assert(proto_get_u16(sent+2)==7);
 now=400;produce();assert(proto_get_u16(sent+2)==7);
 now=600;produce();assert(!(proto_get_u16(sent+2)&7) && sent[1]==FUSION_BLIND && !proto_get_u16(sent+6));
 snapshot.stampMs=20;now=800;produce();assert(proto_get_u16(sent+2)==7);
 up=0;produce();assert(sends==4);up=1;
 snapshot.stampMs=30;now=0xfffffff0u;produce();now=20;produce();assert(proto_get_u16(sent+2)==7);
 now=300;produce();assert(!(proto_get_u16(sent+2)&7));
 puts("CPU2 frozen publisher, reconnect, wrap freshness PASS");return 0;
}
"""
# C raw string above needs ordinary C quote escapes, not doubled backslashes.
model=model.replace('\\\"','\\"')
with tempfile.TemporaryDirectory() as tmp:
 src=Path(tmp)/"bridge.c";exe=Path(tmp)/("bridge.exe" if os.name=="nt" else "bridge")
 src.write_text(model,encoding="utf-8")
 subprocess.run([os.environ.get("CC","gcc"),"-std=c99","-Wall","-Wextra","-Werror","-I",str(ROOT/"tc275_car"),str(src),str(ROOT/"tc275_car/app/fusion.c"),"-lm","-o",str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
