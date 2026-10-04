#include "app/diag_txn.h"
#include "mw/sf/sf_frame.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static DT_Engine s;
static DT_Input in;
static uint8_t q[16];
static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while(0)
typedef struct { uint8_t cid,len,b[32]; } Frame;
static Frame frames[4096];
static unsigned count;
static int blockControl,blockData;
static uint8_t send(void *unused,uint8_t cid,const uint8_t *b,uint8_t len)
{
    (void)unused;
    if((cid==DT_CONTROL_CID && blockControl) || (cid==DT_DATA_CID && blockData)) return 0;
    CHECK(count<4096 && len>=18 && len<=32 && b[0]==1 && !b[3]);
    frames[count].cid=cid; frames[count].len=len; memcpy(frames[count++].b,b,len);
    return 1;
}
static void request(uint8_t op,uint16_t id)
{
    memset(q,0,16); q[0]=DT_SUB; q[1]=1; q[2]=op; DT_put32(q+4,0x12345678u); DT_put16(q+8,id);
}
static void tick(uint32_t now)
{
    unsigned z;
    in.nowMs=now; in.imuStampMs=now; in.imuSeq++;
    if(!in.tof.seq || now-in.tof.sampleStampMs>=70u) {
        in.tof.seq++; in.tof.sampleStampMs=now;
        for(z=0;z<in.tof.zones;z++) { in.tof.distanceMm[z]=1000; in.tof.status[z]=5; in.tof.targets[z]=1; }
    }
    DT_tick(&s,&in,send,NULL);
}
static void drain(void)
{
    unsigned i;
    for(i=0;i<32 && (s.ctrlCount || s.jobs[0].terminalPending);i++) DT_tick(&s,&in,send,NULL);
}
static void reset(void)
{
    memset(&in,0,sizeof(in)); count=0; blockControl=blockData=0;
    in.linkUp=in.stationary=in.imuAlive=in.tof.alive=1;
    in.tof.zones=16; in.accMg[2]=1000; in.gyroMdps[0]=-123456; in.tempCentiC=2345;
    DT_init(&s,0x010300,16,15); tick(100);
    request(DT_HELLO,1); q[10]=q[11]=1;
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(s.session==0x12345678u);
}
static uint8_t lastCode(uint16_t id)
{
    unsigned n=count;
    while(n--) if(frames[n].b[1]==DT_RESULT && DT_u16(frames[n].b+8)==id) return frames[n].b[2];
    return 255;
}
static unsigned body(uint16_t id,uint8_t kind,uint8_t *out)
{
    unsigned i,n=0; uint16_t seq=65535; uint8_t seen[32]={0},total=0;
    /* Pick the most recent complete record with this kind/request. */
    for(i=count;i>0;i--) if(frames[i-1].b[1]==kind && DT_u16(frames[i-1].b+8)==id) {
        seq=DT_u16(frames[i-1].b+10); total=frames[i-1].b[17]; break;
    }
    for(i=0;i<count;i++) {
        Frame *f=&frames[i]; uint8_t index=f->b[16];
        if(f->b[1]!=kind || DT_u16(f->b+8)!=id || DT_u16(f->b+10)!=seq) continue;
        CHECK(index<total && total<=32 && f->b[17]==total);
        if(!seen[index]) { memcpy(out+index*14u,f->b+18,f->len-18u); n+=f->len-18u; seen[index]=1; }
    }
    for(i=0;i<total;i++) CHECK(seen[i]);
    return n;
}
static uint32_t tag(uint16_t id,uint8_t kind,uint8_t key)
{
    uint8_t b[448]; unsigned len=body(id,kind,b),i;
    uint32_t value=0;
    for(i=0;i<len;) {
        unsigned z,n=b[i+1]; CHECK(i+2+n<=len);
        if(b[i]==key) { for(z=0;z<n && z<4;z++) value|=(uint32_t)b[i+2+z]<<(8*z); return value; }
        i+=2+n;
    }
    CHECK(0); return 0;
}
static void capture(uint16_t id,uint8_t mask,uint16_t duration)
{
    request(DT_CAPTURE,id); q[10]=mask; q[11]=(mask&1)?50:0; DT_put16(q+12,duration);
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(id)==255);
}
static void test_wire(void)
{
    uint8_t bytes[32],wire[40]; int16_t n;
    const uint8_t golden[]={0x5a,1,1,0,0,17,0,3,0x53,0x60,1,1,0,0x78,0x56,0x34,0x12,1,0,1,1,0,0,0,0,0x72,0x96,0};
    request(DT_HELLO,1); q[10]=q[11]=1; bytes[0]=0x53; memcpy(bytes+1,q,16);
    n=SF_build(1,0,0,3,bytes,17,wire,sizeof(wire));
    CHECK(n==28); CHECK(!memcmp(wire,golden,sizeof(golden)));
    CHECK(SF_crc16(wire,25)==((uint16_t)wire[25]<<8 | wire[26]));
    CHECK(DT_rejection(bytes,q,9,DT_BUSY,10)==0);
    CHECK(DT_rejection(bytes,q,16,DT_BUSY,10)==24 && bytes[2]==DT_BUSY && DT_u32(bytes+4)==0x12345678);
}
static void test_admission(void)
{
    uint8_t save[16]; unsigned before;
    reset(); request(DT_CAPS,2); CHECK(DT_submit(&s,q,16,&in)); drain();
    CHECK(tag(2,DT_CAPS_RECORD,1)==63 && tag(2,DT_CAPS_RECORD,2)==0x010300);
    before=count; CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(count>before && s.highWater==2);
    q[10]=1; CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(2)==DT_ID_CONFLICT);
    request(0xee,3); CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(3)==DT_BAD_OPCODE);
    request(DT_CAPTURE,4); q[10]=1; q[11]=50; DT_put16(q+12,100); q[3]=1;
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(4)==DT_BAD_ARGUMENT);
    q[3]=0; q[1]=2; CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(4)==DT_BAD_SCHEMA);
    q[1]=1; CHECK(DT_submit(&s,q,15,&in)); drain(); CHECK(lastCode(4)==DT_BAD_LENGTH);
    reset(); in.stationary=0; request(DT_CAPTURE,2); q[10]=1; q[11]=50; DT_put16(q+12,100);
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(2)==DT_MOTION_ACTIVE);
    reset(); capture(2,3,200); memcpy(save,q,16);
    CHECK(DT_submit(&s,save,16,&in)); drain(); CHECK(s.jobs[s.active].start==100);
    request(DT_CAPTURE,3); q[10]=1; q[11]=50; DT_put16(q+12,100);
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(3)==DT_BUSY);
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(3)==DT_BUSY);
    reset(); s.ctrlCount=6; s.controls[0].kind=DT_ACK;
    request(DT_CAPS,2); CHECK(!DT_submit(&s,q,16,&in)); CHECK(s.highWater==1);
}
static void test_capture_and_query(void)
{
    unsigned now; uint8_t b[448]; uint8_t saved[16];
    reset(); capture(2,3,200); memcpy(saved,q,16);
    for(now=110;now<=320;now+=10) tick(now);
    drain(); CHECK(lastCode(2)==DT_OK); CHECK(s.active==255);
    CHECK(tag(2,DT_RESULT,11)==10 && tag(2,DT_RESULT,12)==2 && tag(2,DT_RESULT,13)==0);
    CHECK(body(2,DT_IMU_RECORD,b)==20); CHECK(DT_u16(b+4)==1000 && (int32_t)DT_u32(b+6)==-123456);
    CHECK(body(2,DT_TOF_RECORD,b)==68 && b[0]==4 && b[1]==16 && DT_u16(b+4)==1000);
    CHECK(DT_submit(&s,saved,16,&in)); drain(); CHECK(lastCode(2)==DT_OK && s.active==255);
    request(DT_STATUS,3); DT_put16(q+10,2); q[12]=1;
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(3)==DT_OK);
    CHECK(tag(3,DT_STATUS_RECORD,9)==DT_COMPLETED && tag(3,DT_STATUS_RECORD,17)==2);
    request(DT_CANCEL,4); DT_put16(q+10,2); CHECK(DT_submit(&s,q,16,&in)); drain();
    CHECK(tag(4,DT_RESULT,9)==DT_COMPLETED && lastCode(2)==DT_OK);
}
static void test_failures(void)
{
    uint8_t first[32]; unsigned before;
    reset(); capture(2,1,100); blockControl=1;
    request(DT_STATUS,3); CHECK(DT_submit(&s,q,16,&in));
    first[0]=s.controls[s.ctrlTail].kind; before=s.controls[s.ctrlTail].index;
    tick(110); CHECK(s.controls[s.ctrlTail].kind==first[0] && s.controls[s.ctrlTail].index==before);
    blockControl=0; drain();
    blockData=1; tick(120); blockData=0; drain();
    CHECK(lastCode(2)==DT_OVERFLOW && tag(2,DT_RESULT,13)>0);
    /* A short frozen source is still within the 100 ms health window, but
     * cannot falsely complete a full-rate 100 ms IMU acquisition. */
    reset(); capture(2,1,100); tick(110); in.nowMs=200;
    DT_tick(&s,&in,send,NULL); drain();
    CHECK(lastCode(2)==DT_OVERFLOW && tag(2,DT_RESULT,11)==5 && tag(2,DT_RESULT,13)==8);
    reset(); capture(2,1,500); in.nowMs=210; DT_tick(&s,&in,send,NULL); drain(); CHECK(lastCode(2)==DT_STALE);
    reset(); capture(2,2,500); in.linkUp=0; DT_tick(&s,&in,send,NULL); drain(); CHECK(lastCode(2)==DT_LINK_LOST && !s.session);
    reset(); capture(2,1,2000); tick(1100); drain(); CHECK(lastCode(2)==DT_SESSION_LOST && !s.session);
    request(DT_KEEPALIVE,3); CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(3)==DT_SESSION_LOST);
    reset(); capture(2,1,1000); request(DT_CANCEL,3); DT_put16(q+10,2);
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(2)==DT_CANCELED && lastCode(3)==DT_OK);
    reset(); capture(2,1,1000); DT_abortMotion(&s,&in); drain(); CHECK(lastCode(2)==DT_MOTION_ACTIVE);
    /* A completely blocked control queue must not keep an expired task running. */
    reset(); capture(2,1,2000); s.ctrlCount=8; blockControl=1;
    tick(1100); CHECK(s.active==255 && !s.session && s.jobs[0].terminalPending);
    s.ctrlCount=0; blockControl=0; drain(); CHECK(lastCode(2)==DT_SESSION_LOST);
}
static void test_reference(void)
{
    unsigned now; uint16_t id=3;
    reset(); request(DT_TOF_REFERENCE,2); DT_put16(q+10,1000); DT_put16(q+12,50); q[14]=1; q[15]=5;
    CHECK(DT_submit(&s,q,16,&in)); drain();
    for(now=110;now<=1110;now+=10) {
        if(now%250==0) { in.nowMs=now; request(DT_KEEPALIVE,id++); CHECK(DT_submit(&s,q,16,&in)); }
        tick(now);
    }
    drain(); CHECK(lastCode(2)==DT_OK && tag(2,DT_RESULT,15)==DT_PASS);
    reset(); request(DT_TOF_REFERENCE,2); DT_put16(q+10,1200); DT_put16(q+12,50); q[14]=1; q[15]=5;
    CHECK(DT_submit(&s,q,16,&in)); drain(); id=3;
    for(now=110;now<=1110;now+=10) {
        if(now%250==0) { in.nowMs=now; request(DT_KEEPALIVE,id++); CHECK(DT_submit(&s,q,16,&in)); }
        tick(now);
    }
    drain(); CHECK(lastCode(2)==DT_OK && tag(2,DT_RESULT,15)==DT_FAIL);
}
static void test_wrap_and_cache(void)
{
    uint16_t id; uint8_t saved[16];
    reset(); request(DT_CAPS,2); memcpy(saved,q,16); CHECK(DT_submit(&s,q,16,&in)); drain();
    for(id=3;id<25;id++) { request(DT_KEEPALIVE,id); CHECK(DT_submit(&s,q,16,&in)); drain(); }
    CHECK(DT_submit(&s,saved,16,&in)); drain(); CHECK(lastCode(2)==DT_EXPIRED);
    reset(); in.nowMs=0xfffffff0u; s.leaseMs=in.nowMs;
    in.imuStampMs=in.nowMs; in.tof.sampleStampMs=in.nowMs;
    capture(2,1,100); tick(0xfffffffau); tick(4); tick(24); tick(44); tick(64); tick(84); drain();
    CHECK(lastCode(2)==DT_OK && tag(2,DT_RESULT,14)==100);
    reset(); request(DT_KEEPALIVE,65535); CHECK(DT_submit(&s,q,16,&in)); drain();
    request(DT_KEEPALIVE,2); CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(2)==DT_EXPIRED);
}
static void test_resolution_policy_and_expiry(void)
{
    unsigned now; uint8_t b[448]; uint8_t saved[16];
    reset(); s.tofZones=64; in.tof.zones=64;
    request(DT_CAPTURE,2); q[10]=3; q[11]=100; DT_put16(q+12,500);
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(2)==DT_BAD_ARGUMENT);
    request(DT_TOF_REFERENCE,3); DT_put16(q+10,1000); DT_put16(q+12,50); q[14]=66;
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(3)==DT_BAD_ARGUMENT);
    reset(); s.tofZones=64; in.tof.zones=64;
    capture(3,2,300);
    for(now=110;now<=450;now+=10) tick(now);
    drain(); CHECK(lastCode(3)==DT_OK);
    CHECK(body(3,DT_TOF_RECORD,b)==260 && b[0]==8 && b[1]==64 && DT_u16(b+256)==1000);
    reset(); request(DT_IMU_STATIC,2); q[10]=1; q[11]=25; DT_put16(q+12,200); q[14]=6;
    CHECK(DT_submit(&s,q,16,&in)); drain();
    for(now=110;now<=310;now+=10) tick(now);
    drain(); CHECK(lastCode(2)==DT_OK && tag(2,DT_RESULT,15)==DT_UNASSESSED && tag(2,DT_RESULT,24)==6);
    reset(); capture(2,2,500); tick(110); in.tof.seq+=4; in.tof.sampleStampMs=180; in.nowMs=180;
    DT_tick(&s,&in,send,NULL); tick(600); drain();
    CHECK(lastCode(2)==DT_OVERFLOW && tag(2,DT_RESULT,13)>0);
    reset(); capture(2,1,100); memcpy(saved,q,16); tick(110); tick(210); drain();
    in.nowMs=30300; s.leaseMs=in.nowMs; CHECK(DT_submit(&s,saved,16,&in)); drain(); CHECK(lastCode(2)==DT_EXPIRED);
    /* Cache pressure cannot evict a protected terminal inside 30 seconds. */
    reset();
    for(now=0;now<4;now++) { capture((uint16_t)(2+now),1,100); DT_abortMotion(&s,&in); drain(); }
    request(DT_CAPTURE,6); q[10]=1; q[11]=50; DT_put16(q+12,100);
    CHECK(DT_submit(&s,q,16,&in)); drain(); CHECK(lastCode(6)==DT_BUSY);
    /* Mean=1001.5 is outside a 1 mm tolerance even though integer truncation is 1001. */
    reset(); request(DT_TOF_REFERENCE,2); DT_put16(q+10,1000); DT_put16(q+12,1); q[14]=1;
    CHECK(DT_submit(&s,q,16,&in)); drain();
    s.jobs[0].tofTotal=s.jobs[0].tofValid=10; s.jobs[0].tofSum=10015;
    s.jobs[0].tofSquare=10030025; in.nowMs=1100; s.leaseMs=1100;
    in.tof.sampleStampMs=1100; DT_tick(&s,&in,send,NULL); drain();
    CHECK(lastCode(2)==DT_OK && tag(2,DT_RESULT,15)==DT_FAIL);
}
int main(void)
{
    test_wire(); test_admission(); test_capture_and_query(); test_failures();
    test_reference(); test_wrap_and_cache();
    test_resolution_policy_and_expiry();
    printf("diag transaction tests: %u checks passed\n",checks); return 0;
}
