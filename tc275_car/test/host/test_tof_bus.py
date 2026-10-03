"""Compile the production bus functions against a request-gated I2C model.

No copy of the algorithm: extract the bus section of bsp/tof.c unchanged.
Run: python test/host/test_tof_bus.py [--cc path/to/gcc]
This verifies software ordering and deadlines, not electrical timing.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

MODEL = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bsp/tof.h"
typedef unsigned Ifx_SizeT;
typedef enum { IfxI2c_ProtocolInterruptSource_arbitrationLost=3,
 IfxI2c_ProtocolInterruptSource_notAcknowledgeReceived=4,
 IfxI2c_ProtocolInterruptSource_transmissionEnd=5,
 IfxI2c_ProtocolInterruptSource_receiveMode=6 } IfxI2c_ProtocolInterruptSource;
#define IFX_I2C_RIS_LSREQ_INT_OFF 0
#define IFX_I2C_RIS_SREQ_INT_OFF 1
#define IFX_I2C_RIS_LBREQ_INT_OFF 2
#define IFX_I2C_RIS_BREQ_INT_OFF 3
#define IfxI2c_BusStatus_idle 0
typedef struct {
 struct { unsigned U; } RIS, ERRIRQSS, RXD, PIRQSS;
 struct { struct { unsigned FFS; } B; } FFSSTAT;
 struct { struct { unsigned BS; } B; } BUSSTAT;
 struct { struct { unsigned SETEND; } B; } ENDDCTRL;
} Ifx_I2C;
static Ifx_I2C module;
static unsigned MODULE_P02;
#define TOF_SCL_PORT (&MODULE_P02)
#define TOF_SDA_PORT (&MODULE_P02)
#define TOF_SCL_PIN 5u
#define TOF_SDA_PIN 4u
static unsigned gpio[6]={0,1,1,1,1,1}, stuck_sda, stuck_scl, release_after, edges, delays;
#define IfxPort_OutputMode_openDrain 1u
#define IfxPort_OutputIdx_general 2u
#define pdMS_TO_TICKS(x) (x)
static void vTaskDelay(unsigned n) { assert(n==1); ++delays; }
static boolean IfxPort_getPinState(unsigned *p, unsigned pin) {
 (void)p; return gpio[pin] && !(pin==TOF_SCL_PIN?stuck_scl:stuck_sda);
}
static void IfxPort_setPinHigh(unsigned *p, unsigned pin) {
 (void)p;
 if (pin==TOF_SCL_PIN && !gpio[TOF_SCL_PIN] && !stuck_scl) {
  ++edges;
  if (release_after && edges>=release_after) stuck_sda=0;
 }
 gpio[pin]=1;
}
static void IfxPort_setPinLow(unsigned *p, unsigned pin) { (void)p; gpio[pin]=0; }
static void IfxPort_setPinModeOutput(unsigned *p, unsigned pin, unsigned mode, unsigned idx) {
 (void)p; (void)pin; assert(mode==IfxPort_OutputMode_openDrain && idx==IfxPort_OutputIdx_general);
}
#define TOF_I2C (&module)
static uint8 g_txBuf[TOF_TX_MAX];
static unsigned tick, protocol, tps, remaining, mrps, received, acknowledgements;
static unsigned fault, stall, rx, tx_request, irq_off, preempt_tx, rxmode_delay;
static boolean IfxCpu_disableInterrupts(void) { assert(!irq_off); irq_off=1; return TRUE; }
static void IfxCpu_restoreInterrupts(boolean enabled) { assert(enabled && irq_off); irq_off=0; }
static uint8 wire[128];
static unsigned wire_len;
#define portTICK_PERIOD_MS 1u
static unsigned xTaskGetTickCount(void) {
 assert(!irq_off);
 if (preempt_tx && wire_len && remaining && !rx) module.ERRIRQSS.U |= 4u;
 ++tick;
 if (module.ENDDCTRL.B.SETEND) {
  protocol |= 1u<<5; module.BUSSTAT.B.BS=0;
  module.ENDDCTRL.B.SETEND=0;
 }
 if (rx && !module.RIS.U && received < mrps && !stall) {
  unsigned n=mrps-received, word=0;
  if (n>4) n=4;
  for (unsigned k=0;k<n;++k) word |= (0xa0u+received+k)<<(8*k);
  module.RXD.U=word; received+=n; module.RIS.U=1;
 }
 return tick;
}
static boolean IfxI2c_getProtocolInterruptSourceStatus(Ifx_I2C *p, IfxI2c_ProtocolInterruptSource s) {
 if (s==IfxI2c_ProtocolInterruptSource_receiveMode && rxmode_delay && (protocol & (1u<<6))) {
  assert(mrps==4);
  if (!received) { received=4; p->RXD.U=0xa3a2a1a0u; p->RIS.U=1; }
  assert(received==4 && p->RIS.U);
  rxmode_delay=0;
  return FALSE; /* Data arrives just after the mode status was sampled. */
 }
 p->PIRQSS.U=protocol; return (protocol & (1u<<s)) != 0;
}
static void IfxI2c_clearProtocolInterruptSource(Ifx_I2C *p, IfxI2c_ProtocolInterruptSource s) {
 (void)p; protocol &= ~(1u<<s);
}
static void IfxI2c_clearAllProtocolInterruptSources(Ifx_I2C *p) { (void)p; protocol=0; }
static void IfxI2c_clearAllErrorInterruptSources(Ifx_I2C *p) { p->ERRIRQSS.U=0; }
static boolean IfxI2c_busIsFree(Ifx_I2C *p) { return p->BUSSTAT.B.BS==0; }
static void IfxI2c_setTransmitPacketSize(Ifx_I2C *p, Ifx_SizeT n) {
 assert(!p->RIS.U); tps=n; remaining=n; rx=0; p->BUSSTAT.B.BS=1;
}
static void IfxI2c_setReceivePacketSize(Ifx_I2C *p, Ifx_SizeT n) { (void)p; mrps=n; received=0; }
static void IfxI2c_writeFifo(Ifx_I2C *p, unsigned word) {
 assert(!p->RIS.U); assert(remaining);
 unsigned n=remaining>4?4:remaining;
 if (remaining==tps) rx=(word & 1u) && tps==1;
 for (unsigned k=0;k<n;++k) wire[wire_len++]=(uint8)(word>>(8*k));
 remaining-=n; tx_request=1;
 if (fault==1) protocol|=1u<<4;
 if (fault==2) protocol|=1u<<3;
 if (fault==3) p->ERRIRQSS.U=1;
 if (!stall) p->RIS.U=1;
 /* TX_END deliberately unavailable until the FIFO request is serviced. */
}
static void clear_request(Ifx_I2C *p, unsigned bit) {
 if (!(p->RIS.U & (1u<<bit))) return;
 p->RIS.U &= ~(1u<<bit); ++acknowledgements;
 if (tx_request) {
  tx_request=0;
  if (!remaining) protocol |= 1u<<(rx?6:5);
 } else if (rx && received==mrps) protocol |= 1u<<5;
}
static void IfxI2c_clearLastSingleRequestInterruptSource(Ifx_I2C *p) { clear_request(p,0); }
static void IfxI2c_clearSingleRequestInterruptSource(Ifx_I2C *p) { clear_request(p,1); }
static void IfxI2c_clearLastBurstRequestInterruptSource(Ifx_I2C *p) { clear_request(p,2); }
static void IfxI2c_clearBurstRequestInterruptSource(Ifx_I2C *p) { clear_request(p,3); }
'''

CASES = r'''
static void reset(void) {
 memset(&module,0,sizeof module); tick=protocol=tps=remaining=mrps=received=0;
 acknowledgements=fault=stall=rx=tx_request=wire_len=irq_off=preempt_tx=rxmode_delay=0;
}
int main(void) {
 uint8 payload[TOF_WRITE_CHUNK], data[TOF_READ_CHUNK];
 memset(payload,0x55,sizeof payload);
 reset(); assert(tof_writeReg(0x52,0x7fff,payload,1)==TOF_OK);
 assert(wire_len==4 && !memcmp(wire,"\x52\x7f\xff\x55",4));
 assert(acknowledgements==1 && module.BUSSTAT.B.BS==0);
 reset(); assert(tof_writeReg(0x52,0x1234,payload,sizeof payload)==TOF_OK);
 assert(acknowledgements==7 && wire_len==27);
 reset(); preempt_tx=1;
 assert(tof_writeReg(0x52,0x1234,payload,sizeof payload)==TOF_OK);
 assert(!irq_off && wire_len==27);
 reset(); rxmode_delay=1;
 assert(tof_readReg(0x52,0x2c00,data,4)==TOF_OK);
 assert(data[0]==0xa0 && data[3]==0xa3);
 for (unsigned n=1;n<=TOF_READ_CHUNK;++n) {
  reset(); memset(data,0,sizeof data);
  assert(tof_readReg(0x52,0x7fff,data,n)==TOF_OK);
  assert(wire_len==4 && !memcmp(wire,"\x52\x7f\xff\x53",4));
  assert(acknowledgements==2+(n+3)/4 && module.BUSSTAT.B.BS==0);
  for (unsigned k=0;k<n;++k) assert(data[k]==0xa0u+k);
 }
 for (unsigned f=1;f<=3;++f) {
  reset(); fault=f; memset(data,0xff,sizeof data);
  assert(tof_readReg(0x52,0,data,4)==(f==1?TOF_ERR_NO_ACK:TOF_ERR_BUS));
  assert(tick<TOF_XFER_TIMEOUT_MS);
  assert(g_busDiagValid && g_busDiag[0]==3 && g_busDiag[1]==1);
  assert(g_busDiag[9]==1 && g_busDiag[10]==1);
  assert(g_busDiag[11]==3 && g_busDiag[12]==0x52);
  if (f==1) assert(g_busDiag[4] & (1u<<4));
  if (f==2) assert(g_busDiag[4] & (1u<<3));
  if (f==3) assert(g_busDiag[5]==1);
  assert(data[0]==0 && data[3]==0);
 }
 reset(); stall=1; assert(tof_writeReg(0x52,0,payload,1)==TOF_ERR_TIMEOUT);
 assert(tick>=TOF_XFER_TIMEOUT_MS && tick<3*TOF_XFER_TIMEOUT_MS);
 assert(g_busDiag[0]==4); /* STOP must not overwrite original fault. */
 reset(); module.FFSSTAT.B.FFS=8;
 assert(tof_writeReg(0x52,0,payload,1)==TOF_ERR_TIMEOUT);
 assert(g_busDiag[0]==2);
 reset(); module.BUSSTAT.B.BS=1; memset(data,0xff,sizeof data);
 assert(tof_readReg(0x52,0,data,4)==TOF_ERR_BUS);
 assert(g_busDiag[0]==1 && data[0]==0 && data[3]==0);
 reset(); assert(tof_writeReg(0x52,0xffff,payload,2)==TOF_ERR_PARAM);
 assert(wire_len==0);
 assert(TOF_wireAddrHiRead(0x7fff)==0x7f);
 assert(TOF_wireAddrHiRead(0x0000)==0);
 assert(tof_clearBusPins()==0 && g_busClear[1]==0);
 stuck_sda=1; release_after=9; edges=delays=0;
 assert(tof_clearBusPins()==1 && g_busClear[1]==9 && g_busClear[3]==1);
 stuck_sda=1; release_after=0; edges=delays=0;
 assert(tof_clearBusPins()==3 && g_busClear[1]==9 && g_busClear[3]==0);
 assert(delays<=23 && gpio[TOF_SCL_PIN] && gpio[TOF_SDA_PIN]);
 stuck_scl=1;
 assert(tof_clearBusPins()==2 && g_busClear[1]==0);
 puts("ToF bus: write/read 1..32 B, FIFO handshakes, NACK/AL/FIFO errors, deadlines, wire format PASS");
}
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
    args = parser.parse_args()
    cc = shutil.which(args.cc) or args.cc
    source = (ROOT / 'bsp/tof.c').read_text(encoding='utf-8')
    start = source.index('static uint32 tof_nowMs(')
    end = source.index('/* Module + pads,', start)
    with tempfile.TemporaryDirectory(prefix='tof_bus_') as tmp:
        c = Path(tmp) / 'test.c'
        exe = Path(tmp) / 'test.exe'
        clear_start = source.index('static uint32 g_busClear[4];')
        clear_end = source.index('/* One boot-only GPIO address probe,', clear_start)
        c.write_text(MODEL + source[start:end] + source[clear_start:clear_end] + CASES, encoding='utf-8')
        subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'test/host/stub'), '-I', str(ROOT),
                        str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    main()
