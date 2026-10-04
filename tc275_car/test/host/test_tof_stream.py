"""Compile production ranging step unchanged, mock ULD and time only."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "bsp/tof.c").read_text(encoding="utf-8")
step = source.split("static void tof_rangingStep(void)", 1)[1].split("void TOF_init(void)", 1)[0]
model = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "app/fusion.h"
typedef uint8_t uint8; typedef uint32_t uint32;
#define FALSE 0
#define TOF_ST_PROBE 1
#define TOF_ZONE_COUNT 16
#define VL53L5CX_STATUS_OK 0
static int g_dev,g_alive=1,g_state;
static unsigned g_frameMs,g_reprobeMs,g_failRun,g_frameCount,now,readyValue,statusValue,published,errors;
static FusionTof g_snapshot,wire;
static struct {int16_t distance_mm[16];uint8_t target_status[16],nb_target_detected[16];} g_results;
static unsigned tof_nowMs(void){return now;}
static void XCORE_logln(const char *p){(void)p;}
static void XCORE_tofPublish(const FusionTof *p){wire=*p;published++;}
static void tof_dataFailed(uint8_t st){assert(st);errors++;}
static uint8_t vl53l5cx_check_data_ready(int *dev,uint8_t *r){(void)dev;*r=(uint8_t)readyValue;return (uint8_t)statusValue;}
static uint8_t vl53l5cx_get_ranging_data(int *dev,void *r){(void)dev;(void)r;return (uint8_t)statusValue;}
static void tof_rangingStep(void)
""" + step + r"""
int main(void){
 now=999;tof_rangingStep();assert(!published && g_alive);
 now=1000;tof_rangingStep();assert(published==1 && !wire.alive && !g_alive && g_state==TOF_ST_PROBE);
 g_alive=1;g_frameMs=now;readyValue=1;g_results.distance_mm[7]=123;
 g_results.target_status[7]=5;g_results.nb_target_detected[7]=1;
 now=1010;tof_rangingStep();assert(wire.seq==1 && wire.stampMs==1010 && wire.alive);
 assert(wire.distanceMm[7]==123 && wire.targets[7]==1 && wire.status[7]==5);
 statusValue=1;tof_rangingStep();assert(errors==1 && published==2);
 statusValue=0;readyValue=0;g_frameMs=0xffffff00u;now=20;tof_rangingStep();assert(published==2);
 puts("ToF stream: complete snapshot, silent timeout, failure isolation, clock wrap PASS");return 0;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    src=Path(tmp)/"stream.c"
    exe=Path(tmp)/("stream.exe" if os.name=="nt" else "stream")
    src.write_text(model,encoding="utf-8")
    subprocess.run([os.environ.get("CC","gcc"),"-std=c99","-Wall","-Wextra","-Werror","-I",str(ROOT),str(src),"-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
