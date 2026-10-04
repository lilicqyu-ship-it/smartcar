/* Compile the actual CPU0 adapter with sensor/xcore/robot boundaries mocked. */
#include "mw/diag/diag_service.c"
#include <assert.h>
#include <stdio.h>
static uint32 now=100;
static XcoreImu sample;
static FusionTof ranges;
static XcoreEncoder encoder;
static XcoreJog jog;
static RobotSpeeds wanted;
static sint16 targetLeft,targetRight,dutyLeft,dutyRight;
static boolean bench,up=TRUE,dataRoom=TRUE,ctrlRoom=TRUE,pending;
static XcoreCmdMsg command;
static unsigned dataEvents,controlEvents,pops;
uint32 STIME_nowMs(void) { return now; }
uint32 TOF_errCount(void) { return 0; }
void XCORE_imuRead(XcoreImu *p) { *p=sample; }
void XCORE_tofRead(FusionTof *p) { *p=ranges; }
void XCORE_encoderRead(XcoreEncoder *p) { *p=encoder; }
uint32 XCORE_jogGet(XcoreJog *p) { *p=jog; return jog.jogSeq; }
uint32 XCORE_motorGetTarget(sint16 *l,sint16 *r,boolean *e) { *l=targetLeft;*r=targetRight;*e=FALSE;return 1; }
void XCORE_motorStatusGet(sint16 *l,sint16 *r) { *l=dutyLeft;*r=dutyRight; }
boolean XCORE_benchIsActive(void) { return bench; }
boolean XCORE_linkRead(uint32 *stamp) { *stamp=100; return up; }
RobotSpeeds ROBOT_getSpeeds(void) { return wanted; }
boolean XCORE_diagCmdPeek(XcoreCmdMsg *p) { *p=command; return pending; }
void XCORE_diagCmdPop(void) { pending=FALSE; pops++; }
boolean XCORE_evtPush(const XcoreEvtFrame *p) { assert(p->cid==DT_CONTROL_CID); if(ctrlRoom) controlEvents++;return ctrlRoom; }
boolean XCORE_dataEvtPush(const XcoreEvtFrame *p) { assert(p->cid==DT_DATA_CID); if(dataRoom)dataEvents++;return dataRoom; }
static void submit(uint8 op,uint16 id)
{
    memset(&command,0,sizeof(command)); command.cmd=0x53; command.len=16;
    command.data[0]=0x60; command.data[1]=1; command.data[2]=op;
    DT_put32(command.data+4,123); DT_put16(command.data+8,id); pending=TRUE;
}
static void fresh(void)
{
    sample.alive=TRUE; sample.whoAmI=0x71; sample.seq++; sample.stampMs=now;
    ranges.alive=1; ranges.zones=TOF_ZONE_COUNT; ranges.seq++; ranges.sampleStampMs=now;
}
static void capture(uint16 id)
{
    submit(DT_CAPTURE,id); command.data[10]=3; command.data[11]=50; DT_put16(command.data+12,100);
    DIAG_tick();
}
int main(void)
{
    unsigned before; uint8 motor[2]={1,0};
    DIAG_init(); fresh(); submit(DT_HELLO,1); command.data[10]=command.data[11]=1;
    DIAG_tick(); assert(pops==1 && g_diag.session==123);
    bench=TRUE; capture(2); assert(g_diag.active==255); bench=FALSE;
    wanted.left=1; capture(3); assert(g_diag.active==255); wanted.left=0;
    targetRight=1; capture(4); assert(g_diag.active==255); targetRight=0;
    dutyLeft=1; capture(5); assert(g_diag.active==255); dutyLeft=0;
    encoder.vMeasLeftMmS=1; capture(6); assert(g_diag.active==255); encoder.vMeasLeftMmS=0;
    jog.duty[3]=1; capture(7); assert(g_diag.active==255); jog.duty[3]=0;
    for(before=0;before<20;before++) DIAG_tick();
    capture(8); assert(g_diag.active!=255);
    now=110; fresh(); DIAG_tick(); assert(dataEvents && controlEvents);
    DIAG_onCommand(PROTO_CMD_HEARTBEAT,NULL,0); assert(g_diag.active!=255);
    DIAG_onCommand(PROTO_CMD_SET_SPEED,motor,2); assert(g_diag.active==255);
    assert(g_diag.jobs[0].cache.records[0].code==DT_MOTION_ACTIVE);
    /* Stale CPU2 publisher is independently detected even if cached up=true. */
    now=250; fresh(); snapshot(); assert(!g_input.linkUp);
    /* CPU0 backpressure retains the inbound diagnostic request. */
    now=100; g_diag.ctrlCount=6; ctrlRoom=FALSE; submit(DT_CAPS,9); before=pops;
    DIAG_tick(); assert(pending && pops==before);
    puts("production CPU0 diagnostic adapter: ownership, stationary gates, motion abort, link freshness and retained request passed");
    return 0;
}
