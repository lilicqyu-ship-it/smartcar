#include "mw/diag/diag_service.h"
#include "app/diag_txn.h"
#include "app/robot.h"
#include "mw/xcore/xcore.h"
#include "mw/app_version.h"
#include "bsp/stime.h"
#include "bsp/tof.h"
#include <string.h>

/* Static RAM, not the 10 ms task's stack. No allocation or sensor HW access. */
static DT_Engine g_diag;
static DT_Input g_input;
static void snapshot(void)
{
    XcoreImu imu; XcoreEncoder enc; XcoreJog jog;
    RobotSpeeds speeds;
    sint16 left,right,dutyLeft,dutyRight;
    boolean estop;
    uint32 linkStamp;
    uint8 i;
    g_input.nowMs=STIME_nowMs();
    XCORE_imuRead(&imu); XCORE_tofRead(&g_input.tof);
    g_input.imuSeq=imu.seq; g_input.imuStampMs=imu.stampMs;
    g_input.imuAlive=imu.alive; g_input.imuErrors=imu.errCount;
    g_input.tempCentiC=imu.tempCentiC;
    memcpy(g_input.accMg,imu.accMilliG,sizeof(g_input.accMg));
    memcpy(g_input.gyroMdps,imu.gyroMilliDps,sizeof(g_input.gyroMdps));
    g_input.tofErrors=TOF_errCount();
    g_input.linkUp=(uint8)XCORE_linkRead(&linkStamp);
    if(g_input.nowMs-linkStamp>100u) g_input.linkUp=0u;
    speeds=ROBOT_getSpeeds(); XCORE_encoderRead(&enc); XCORE_jogGet(&jog);
    (void)XCORE_motorGetTarget(&left,&right,&estop);
    XCORE_motorStatusGet(&dutyLeft,&dutyRight);
    g_input.stationary=(uint8)(!speeds.left && !speeds.right && !left && !right &&
        !dutyLeft && !dutyRight && !enc.vMeasLeftMmS && !enc.vMeasRightMmS && !XCORE_benchIsActive());
    for(i=0;i<CALIB_REC_WHEELS;i++) if(jog.duty[i]) g_input.stationary=0u;
}
static uint8 emit(void *ctx,uint8 cid,const uint8 *bytes,uint8 len)
{
    XcoreEvtFrame f;
    (void)ctx;
    f.type=5u; f.cid=cid; f.len=len; memcpy(f.payload,bytes,len);
    return (uint8)(cid==DT_DATA_CID?XCORE_dataEvtPush(&f):XCORE_evtPush(&f));
}
void DIAG_init(void)
{
    DT_init(&g_diag,((uint32)APP_VERSION_MAJOR<<16)|((uint32)APP_VERSION_MINOR<<8)|APP_VERSION_PATCH,
            TOF_ZONE_COUNT,15u);
}
boolean DIAG_command(const uint8 *bytes,uint8 len)
{
    snapshot(); return (boolean)DT_submit(&g_diag,bytes,len,&g_input);
}
void DIAG_tick(void)
{
    XcoreCmdMsg msg;
    /* Tick before admission expires old leases and drains reliable control
     * fragments. One request per tick bounds work under malicious bursts. */
    snapshot(); DT_tick(&g_diag,&g_input,emit,NULL_PTR);
    if(XCORE_diagCmdPeek(&msg) && DIAG_command(msg.data,msg.len)) XCORE_diagCmdPop();
}
void DIAG_onCommand(uint8 cmd,const uint8 *data,uint8 len)
{
    boolean changes=FALSE;
    if(cmd>=PROTO_CMD_FORWARD && cmd<=PROTO_CMD_ROTATE_RIGHT) changes=TRUE;
    if(cmd==PROTO_CMD_SET_SPEED && len && data &&
        (data[0] || (len>=2u && data[1]))) changes=TRUE;
    if(cmd==PROTO_CMD_DPT_CAL_DIR || cmd==PROTO_CMD_RESET ||
       cmd==PROTO_CMD_DPT_REC_SET || cmd==PROTO_CMD_DPT_REC_CLEAR) changes=TRUE;
    if(cmd==PROTO_CMD_DPT_MOTOR_JOG && data && len==3u && (data[1] || data[2])) changes=TRUE;
    if(changes) { snapshot(); DT_abortMotion(&g_diag,&g_input); }
}
