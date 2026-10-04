#include "app/diag_txn.h"
#include <string.h>

#define NO_JOB 255u
#define LEASE_MS 1000u
#define RETAIN_MS 30000u
static uint8_t fragments(uint16_t len)
{ return len ? (uint8_t)((len+13u)/14u) : 1u; }
static uint8_t zero(const uint8_t *p, uint8_t n)
{ while (n--) if (*p++) return 0u; return 1u; }
static void tlv(DT_Record *r, uint8_t tag, const uint8_t *p, uint8_t n)
{
    /* All producers below have statically bounded layouts <=112 B. */
    if ((uint16_t)(r->len+n+2u)>DT_RECORD_BODY) return;
    r->body[r->len++]=tag; r->body[r->len++]=n;
    if (n) memcpy(r->body+r->len,p,n);
    r->len=(uint16_t)(r->len+n);
}
static void t8(DT_Record *r,uint8_t tag,uint8_t v) { tlv(r,tag,&v,1u); }
static void t16(DT_Record *r,uint8_t tag,uint16_t v)
{ uint8_t b[2]; DT_put16(b,v); tlv(r,tag,b,2u); }
static void t32(DT_Record *r,uint8_t tag,uint32_t v)
{ uint8_t b[4]; DT_put32(b,v); tlv(r,tag,b,4u); }
static uint16_t health(const DT_Input *in)
{
    uint16_t h=0u;
    if(in->imuAlive) h|=1u;
    if(in->imuAlive && in->imuSeq && in->nowMs-in->imuStampMs<=100u) h|=2u;
    if(in->tof.alive) h|=4u;
    if(in->tof.alive && in->tof.seq && in->nowMs-in->tof.sampleStampMs<=250u) h|=8u;
    return h;
}
static void record(DT_Record *r,uint32_t session,uint16_t id,uint16_t seq,
                   uint8_t kind,uint8_t code,uint32_t now)
{
    memset(r,0,sizeof(*r)); r->session=session; r->request=id; r->seq=seq;
    r->kind=kind; r->code=code; r->stamp=now;
}
static void enqueue(DT_Engine *s,const DT_Record *r)
{
    s->controls[s->ctrlHead]=*r;
    s->ctrlHead=(uint8_t)((s->ctrlHead+1u)%DT_CONTROL_QUEUE); s->ctrlCount++;
}
static void replay(DT_Engine *s,const DT_Cached *c)
{
    uint8_t i;
    for(i=0u;i<c->recordCount;i++) enqueue(s,&c->records[i]);
}
static void remember(DT_Engine *s,const uint8_t *q,uint8_t len,
                     const DT_Record *first,const DT_Record *last)
{
    DT_Cached *c=&s->shortCache[s->shortNext];
    s->shortNext=(uint8_t)((s->shortNext+1u)%DT_SHORT_CACHE);
    memset(c,0,sizeof(*c)); c->used=1u; c->requestLen=len;
    memcpy(c->requestBytes,q,len>16u?16u:len);
    c->records[0]=*first; c->recordCount=1u;
    if(last) { c->records[1]=*last; c->recordCount=2u; }
    replay(s,c);
}
static DT_Cached *lookup(DT_Engine *s,uint32_t session,uint16_t id)
{
    uint8_t i;
    for(i=0u;i<DT_JOB_CACHE;i++) {
        DT_Cached *c=&s->jobs[i].cache;
        if(c->used && DT_u32(c->requestBytes+4)==session && DT_u16(c->requestBytes+8)==id) return c;
    }
    for(i=0u;i<DT_SHORT_CACHE;i++) {
        DT_Cached *c=&s->shortCache[i];
        if(c->used && DT_u32(c->requestBytes+4)==session && DT_u16(c->requestBytes+8)==id) return c;
    }
    return NULL;
}
static void error(DT_Engine *s,const uint8_t *q,uint8_t code,uint32_t now)
{
    DT_Record r;
    record(&r,DT_u32(q+4),DT_u16(q+8),0u,DT_RESULT,code,now);
    t8(&r,9u,DT_FAILED); t8(&r,15u,DT_INCONCLUSIVE); enqueue(s,&r);
}
static uint32_t root64(uint64_t v)
{
    uint64_t bit=(uint64_t)1u<<62, result=0u;
    while(bit>v) bit>>=2;
    while(bit) {
        if(v>=result+bit) { v-=result+bit; result=(result>>1)+bit; }
        else result>>=1;
        bit>>=2;
    }
    return (uint32_t)result;
}
static void summary(DT_Record *r,const DT_Job *j,const DT_Input *in)
{
    uint8_t b[16];
    t8(r,9u,j?j->state:DT_IDLE); t16(r,10u,health(in));
    t32(r,11u,j?j->imuCount:0u); t32(r,12u,j?j->tofCount:0u);
    t32(r,13u,j?j->drops:0u); t32(r,14u,j?(j->state==DT_RUNNING?in->nowMs:j->end)-j->start:0u);
    t8(r,15u,j?j->verdict:DT_UNASSESSED);
    t16(r,16u,j && j->cache.requestBytes[2]==DT_TOF_REFERENCE?1u:0u);
    DT_put32(b,in->imuErrors); DT_put32(b+4,in->tofErrors); tlv(r,20u,b,8u);
    t16(r,21u,r->seq);
    if(j && j->cache.requestBytes[2]==DT_TOF_REFERENCE) {
        uint32_t std=0u; int32_t mean=0;
        if(j->tofValid) {
            int64_t variance;
            mean=(int32_t)(j->tofSum/j->tofValid);
            variance=(j->tofSquare*j->tofValid-j->tofSum*j->tofSum)/
                     ((int64_t)j->tofValid*j->tofValid);
            std=root64((uint64_t)(variance>0?variance:0));
        }
        DT_put16(b,j->reference); DT_put16(b+2,j->tolerance); b[4]=j->zone; b[5]=1u;
        tlv(r,22u,b,6u);
        DT_put32(b,j->tofTotal); DT_put32(b+4,j->tofValid);
        DT_put32(b+8,(uint32_t)mean); DT_put32(b+12,std); tlv(r,23u,b,16u);
    }
    if(j && j->cache.requestBytes[2]==DT_IMU_STATIC) t8(r,24u,j->cache.requestBytes[14]);
}
static void dropStream(DT_Job *j,DT_Stream *p)
{
    if(p->len) j->drops+=(uint32_t)(fragments(p->len)-p->index);
    p->len=0u;
}
static void finish(DT_Engine *s,const DT_Input *in,uint8_t code)
{
    DT_Job *j;
    DT_Record r;
    if(s->active==NO_JOB) return;
    j=&s->jobs[s->active];
    dropStream(j,&s->imuStream); dropStream(j,&s->tofStream);
    j->end=in->nowMs;
    j->state=code==DT_OK?DT_COMPLETED:(code==DT_CANCELED?DT_TASK_CANCELED:DT_FAILED);
    if(code==DT_OK && j->drops) { code=DT_OVERFLOW; j->state=DT_FAILED; }
    j->verdict=code==DT_OK?DT_UNASSESSED:DT_INCONCLUSIVE;
    if(code==DT_OK && j->cache.requestBytes[2]==DT_TOF_REFERENCE) {
        if(j->tofValid<5u || j->tofValid*5u<j->tofTotal*4u ||
           in->tofErrors!=j->tofErrors) j->verdict=DT_INCONCLUSIVE;
        else {
            int64_t n=j->tofValid;
            int64_t varianceNumerator=j->tofSquare*n-j->tofSum*j->tofSum;
            int64_t diff=j->tofSum-(int64_t)j->reference*n;
            if(diff<0) diff=-diff;
            /* Compare exact rational moments: rounding a mean to millimetres
             * must never turn a just-outside-tolerance reference into PASS. */
            j->verdict=diff<=(int64_t)j->tolerance*n &&
                varianceNumerator<=(int64_t)j->tolerance*j->tolerance*n*n?DT_PASS:DT_FAIL;
        }
    }
    record(&r,s->session,DT_u16(j->cache.requestBytes+8),j->nextSeq++,DT_RESULT,code,in->nowMs);
    summary(&r,j,in);
    j->cache.records[0]=r; j->cache.recordCount=1u;
    if(s->ctrlCount<DT_CONTROL_QUEUE) enqueue(s,&r);
    else j->terminalPending=1u;
    s->active=NO_JOB;
}
void DT_init(DT_Engine *s,uint32_t fw,uint8_t zones,uint8_t hz)
{
    memset(s,0,sizeof(*s)); s->active=NO_JOB; s->firmwareVersion=fw;
    s->tofZones=zones; s->tofHz=hz;
}
static DT_Job *jobById(DT_Engine *s,uint16_t id,uint32_t now)
{
    uint8_t i;
    for(i=0;i<DT_JOB_CACHE;i++) {
        DT_Job *j=&s->jobs[i];
        if(j->cache.used && DT_u32(j->cache.requestBytes+4)==s->session &&
           DT_u16(j->cache.requestBytes+8)==id &&
           (j->state==DT_RUNNING || now-j->end<RETAIN_MS)) return j;
    }
    return NULL;
}
static uint8_t freeJob(DT_Engine *s,uint32_t now)
{
    uint8_t i;
    for(i=0;i<DT_JOB_CACHE;i++) if(!s->jobs[i].cache.used ||
        (s->jobs[i].state!=DT_RUNNING && now-s->jobs[i].end>=RETAIN_MS)) return i;
    return NO_JOB;
}
static uint8_t rateOk(uint8_t hz)
{ return hz==10u || hz==20u || hz==25u || hz==50u || hz==100u; }
uint8_t DT_submit(DT_Engine *s,const uint8_t *q,uint8_t len,const DT_Input *in)
{
    uint32_t session;
    uint16_t id;
    uint8_t op,code=DT_OK,i,isJob=0u;
    DT_Record r,last;
    DT_Cached *cached;
    if(!q || len<10u || q[0]!=DT_SUB) return 1u;
    /* Reserve enough slots for cancel's own terminal + original task terminal
     * or a CAPS/STATUS pair; no input mutation on backpressure. */
    if(s->ctrlCount>DT_CONTROL_QUEUE-3u) return 0u;
    session=DT_u32(q+4); id=DT_u16(q+8); op=q[2];
    if(len!=16u) { error(s,q,DT_BAD_LENGTH,in->nowMs); return 1u; }
    if(q[1]!=DT_SCHEMA) { error(s,q,DT_BAD_SCHEMA,in->nowMs); return 1u; }
    if(!session || !id || q[3]) { error(s,q,DT_BAD_ARGUMENT,in->nowMs); return 1u; }
    if(op!=DT_HELLO && (session!=s->session || in->nowMs-s->leaseMs>=LEASE_MS)) {
        error(s,q,DT_SESSION_LOST,in->nowMs); return 1u;
    }
    if(op==DT_HELLO && s->session && session!=s->session) {
        error(s,q,DT_BUSY,in->nowMs); return 1u;
    }
    cached=lookup(s,session,id);
    if(cached) {
        if(op==DT_HELLO && session!=s->session) {
            error(s,q,DT_SESSION_LOST,in->nowMs); return 1u;
        }
        if(cached->requestLen!=len || memcmp(cached->requestBytes,q,16u)) error(s,q,DT_ID_CONFLICT,in->nowMs);
        else {
            DT_Job *j=jobById(s,id,in->nowMs);
            for(i=0u;i<DT_JOB_CACHE;i++) if(cached==&s->jobs[i].cache) isJob=1u;
            if(isJob && !j) error(s,q,DT_EXPIRED,in->nowMs);
            else {
                replay(s,cached);
                if(op==DT_STATUS && q[12]) {
                    DT_Job *target=jobById(s,DT_u16(q+10),in->nowMs);
                    if(target && target->state!=DT_RUNNING) replay(s,&target->cache);
                }
            }
        }
        return 1u;
    }
    if(session==s->session && id<=s->highWater) { error(s,q,DT_EXPIRED,in->nowMs); return 1u; }
    if(op==DT_HELLO) {
        if(q[10]!=1u || q[11]!=1u || !zero(q+12,4u)) code=DT_BAD_ARGUMENT;
        else if(!in->linkUp) code=DT_LINK_LOST;
        else {
            if(s->session!=session) {
                s->highWater=0u; memset(s->shortCache,0,sizeof(s->shortCache));
            }
            s->session=session; s->leaseMs=in->nowMs;
        }
    } else if(op==DT_CAPS || op==DT_KEEPALIVE) {
        if(!zero(q+10,6u)) code=DT_BAD_ARGUMENT;
        else if(op==DT_KEEPALIVE) s->leaseMs=in->nowMs;
    } else if(op==DT_STATUS) {
        if(!zero(q+13,3u) || q[12]>1u || (q[12]==0u && DT_u16(q+10))) code=DT_BAD_ARGUMENT;
        else if(q[12] && !jobById(s,DT_u16(q+10),in->nowMs)) code=DT_EXPIRED;
    } else if(op==DT_CANCEL) {
        DT_Job *target=jobById(s,DT_u16(q+10),in->nowMs);
        if(!zero(q+13,3u) || q[12]>1u || !DT_u16(q+10)) code=DT_BAD_ARGUMENT;
        else if(!target) code=DT_EXPIRED;
        else if(target->state==DT_RUNNING) finish(s,in,DT_CANCELED);
    } else if(op==DT_CAPTURE || op==DT_IMU_STATIC || op==DT_TOF_REFERENCE) {
        uint8_t sensors=op==DT_TOF_REFERENCE?2u:q[10];
        uint8_t hz=op==DT_TOF_REFERENCE?0u:q[11];
        uint16_t duration=op==DT_TOF_REFERENCE?(uint16_t)q[14]*1000u:DT_u16(q+12);
        uint8_t index=freeJob(s,in->nowMs);
        uint16_t h=health(in);
        if(!sensors || (sensors&~3u) || duration<100u || duration>60000u ||
           ((sensors&1u)?!rateOk(hz):hz!=0u)) code=DT_BAD_ARGUMENT;
        else if(op==DT_CAPTURE && !zero(q+14,2u)) code=DT_BAD_ARGUMENT;
        else if(op==DT_IMU_STATIC && (sensors!=1u || q[14]>6u || q[15])) code=DT_BAD_ARGUMENT;
        else if(op==DT_TOF_REFERENCE && (q[14]>60u || DT_u16(q+10)<20u || DT_u16(q+10)>4000u ||
                !DT_u16(q+12) || DT_u16(q+12)>1000u || q[15]>=s->tofZones)) code=DT_BAD_ARGUMENT;
        else if(sensors==3u && s->tofZones==64u && hz>50u) code=DT_BAD_ARGUMENT;
        else if(s->active!=NO_JOB || index==NO_JOB) code=DT_BUSY;
        else if(!in->stationary) code=DT_MOTION_ACTIVE;
        else if(!in->linkUp) code=DT_LINK_LOST;
        else if(((sensors&1u) && (h&3u)!=3u) || ((sensors&2u) && (h&12u)!=12u)) code=DT_NOT_READY;
        else if((sensors&2u) && in->tof.zones!=s->tofZones) code=DT_NOT_READY;
        if(code==DT_OK) {
            DT_Job *j=&s->jobs[index]; uint8_t b[4];
            memset(j,0,sizeof(*j)); j->cache.used=1u; j->cache.requestLen=16u;
            memcpy(j->cache.requestBytes,q,16u); j->state=DT_RUNNING;
            j->start=in->nowMs; j->progressMs=in->nowMs; j->duration=duration;
            j->sensors=sensors; j->hz=hz; j->imuDue=in->nowMs;
            j->imuErrors=in->imuErrors; j->tofErrors=in->tofErrors;
            if(op==DT_TOF_REFERENCE) { j->reference=DT_u16(q+10); j->tolerance=DT_u16(q+12); j->zone=q[15]; }
            record(&r,session,id,j->nextSeq++,DT_ACK,DT_OK,in->nowMs);
            t8(&r,3u,sensors); b[0]=hz; b[1]=(sensors&2u)?s->tofHz:0u;
            DT_put16(b+2,duration); tlv(&r,8u,b,4u);
            j->cache.records[0]=r; j->cache.recordCount=1u;
            enqueue(s,&r); s->active=index; s->highWater=id;
            return 1u;
        }
    } else code=DT_BAD_OPCODE;
    record(&last,session,id,0u,DT_RESULT,code,in->nowMs);
    t8(&last,9u,code==DT_OK?DT_COMPLETED:DT_FAILED);
    if(code==DT_OK && op==DT_CAPS) {
        uint8_t b[5];
        record(&r,session,id,0u,DT_CAPS_RECORD,DT_OK,in->nowMs);
        t32(&r,1u,0x3fu); t32(&r,2u,s->firmwareVersion); t8(&r,3u,3u); t16(&r,4u,60000u);
        b[0]=10u; b[1]=20u; b[2]=25u; b[3]=50u; b[4]=100u; tlv(&r,5u,b,5u);
        b[0]=s->tofZones==64u?8u:4u; b[1]=s->tofHz; tlv(&r,6u,b,2u);
        b[0]=1u; DT_put16(b+1,1u); tlv(&r,7u,b,3u);
        tlv(&r,25u,in->imuInfo,8u);
        last.seq=1u; remember(s,q,len,&r,&last);
    } else if(code==DT_OK && op==DT_STATUS) {
        DT_Job *j=q[12]?jobById(s,DT_u16(q+10),in->nowMs):
                     (s->active==NO_JOB?NULL:&s->jobs[s->active]);
        record(&r,session,id,0u,DT_STATUS_RECORD,DT_OK,in->nowMs);
        summary(&r,j,in); t32(&r,18u,in->imuSeq); t32(&r,19u,in->tof.seq);
        tlv(&r,25u,in->imuInfo,8u);
        if(j) t16(&r,17u,DT_u16(j->cache.requestBytes+8));
        last.seq=1u; remember(s,q,len,&r,&last);
        /* Query of a finished task also replays the immutable original RESULT
         * (original session/request/seq), without restarting sampling. */
        if(q[12] && j && j->state!=DT_RUNNING) replay(s,&j->cache);
    } else {
        if(op==DT_CANCEL) {
            DT_Job *j=jobById(s,DT_u16(q+10),in->nowMs);
            t16(&last,17u,DT_u16(q+10)); if(j) last.body[2]=j->state;
        }
        remember(s,q,len,&last,NULL);
    }
    if(session==s->session) s->highWater=id;
    return 1u;
}
static uint8_t emitFragment(DT_Emit emit,void *ctx,uint8_t cid,uint32_t session,
                           uint16_t request,uint16_t seq,uint32_t stamp,
                           uint8_t kind,uint8_t code,const uint8_t *body,
                           uint16_t len,uint8_t index)
{
    uint8_t frame[32],n; uint16_t offset=(uint16_t)index*14u;
    uint16_t remaining=(uint16_t)(len-offset);
    n=(uint8_t)(remaining>14u?14u:remaining);
    memset(frame,0,18u); frame[0]=1u; frame[1]=kind; frame[2]=code;
    DT_put32(frame+4,session); DT_put16(frame+8,request); DT_put16(frame+10,seq);
    DT_put32(frame+12,stamp); frame[16]=index; frame[17]=fragments(len);
    if(n) memcpy(frame+18,body+offset,n);
    return emit(ctx,cid,frame,(uint8_t)(18u+n));
}
static void sampleImu(DT_Job *j,DT_Stream *p,const DT_Input *in)
{
    uint8_t i;
    p->kind=DT_IMU_RECORD; p->stamp=in->imuStampMs; p->seq=j->nextSeq++;
    p->len=20u; p->index=0u;
    for(i=0;i<3u;i++) { DT_put16(p->body+i*2u,(uint16_t)in->accMg[i]);
        DT_put32(p->body+6u+i*4u,(uint32_t)in->gyroMdps[i]); }
    DT_put16(p->body+18,(uint16_t)in->tempCentiC);
    j->imuSource=in->imuSeq; j->imuCount++;
}
static void sampleTof(DT_Job *j,DT_Stream *p,const DT_Input *in)
{
    uint8_t i;
    p->kind=DT_TOF_RECORD; p->stamp=in->tof.sampleStampMs; p->seq=j->nextSeq++;
    p->len=(uint16_t)(4u+in->tof.zones*4u); p->index=0u;
    p->body[0]=in->tof.zones==64u?8u:4u; p->body[1]=in->tof.zones;
    p->body[2]=in->tof.alive; p->body[3]=0u;
    for(i=0u;i<in->tof.zones;i++) {
        DT_put16(p->body+4u+i*4u,(uint16_t)in->tof.distanceMm[i]);
        p->body[6u+i*4u]=in->tof.status[i]; p->body[7u+i*4u]=in->tof.targets[i];
    }
    j->tofSource=in->tof.seq; j->tofCount++;
    if(j->cache.requestBytes[2]==DT_TOF_REFERENCE) {
        int32_t mm=in->tof.distanceMm[j->zone]; j->tofTotal++;
        if(in->tof.status[j->zone]==5u && in->tof.targets[j->zone] && mm>0) {
            j->tofValid++; j->tofSum+=mm; j->tofSquare+=(int64_t)mm*mm;
        }
    }
}
void DT_abortMotion(DT_Engine *s,const DT_Input *in)
{
    if(s->active!=NO_JOB) finish(s,in,DT_MOTION_ACTIVE);
}
void DT_tick(DT_Engine *s,const DT_Input *in,DT_Emit emit,void *ctx)
{
    uint8_t budget=4u, i;
    DT_Job *j;
    /* Reliable control queue: preserve the exact pending fragment on failure. */
    while(s->ctrlCount && budget) {
        DT_Record *r=&s->controls[s->ctrlTail];
        budget--;
        if(!emitFragment(emit,ctx,DT_CONTROL_CID,r->session,r->request,r->seq,
                         r->stamp,r->kind,r->code,r->body,r->len,r->index)) break;
        if(++r->index==fragments(r->len)) {
            s->ctrlTail=(uint8_t)((s->ctrlTail+1u)%DT_CONTROL_QUEUE); s->ctrlCount--;
        }
    }
    for(i=0u;i<DT_JOB_CACHE && s->ctrlCount<DT_CONTROL_QUEUE;i++) {
        if(s->jobs[i].terminalPending) {
            enqueue(s,&s->jobs[i].cache.records[0]); s->jobs[i].terminalPending=0u;
        }
    }
    if(s->session && (!in->linkUp || in->nowMs-s->leaseMs>=LEASE_MS)) {
        finish(s,in,in->linkUp?DT_SESSION_LOST:DT_LINK_LOST);
        s->session=0u; s->highWater=0u;
        return;
    }
    if(s->active==NO_JOB) return;
    j=&s->jobs[s->active];
    if(!in->stationary) { finish(s,in,DT_MOTION_ACTIVE); return; }
    if(((j->sensors&1u) && (health(in)&3u)!=3u) ||
       ((j->sensors&2u) && ((health(in)&12u)!=12u || in->tof.zones!=s->tofZones))) {
        finish(s,in,DT_STALE); return;
    }
    if(in->nowMs-j->start<j->duration) {
        if((j->sensors&1u) && (int32_t)(in->nowMs-j->imuDue)>=0 && !s->imuStream.len && in->imuSeq!=j->imuSource) {
            uint32_t period=1000u/j->hz;
            uint32_t missed=(in->nowMs-j->imuDue)/period;
            j->imuCount+=missed; j->drops+=missed*2u; j->nextSeq=(uint16_t)(j->nextSeq+missed);
            j->imuDue+=period*(missed+1u); sampleImu(j,&s->imuStream,in);
        }
        if((j->sensors&2u) && !s->tofStream.len && in->tof.seq!=j->tofSource) {
            if(j->tofSource && in->tof.seq-j->tofSource>1u) {
                uint32_t missed=in->tof.seq-j->tofSource-1u;
                /* A reset/backwards sequence is sensor discontinuity, not an enormous count. */
                if(missed>900u) { finish(s,in,DT_STALE); return; }
                j->tofCount+=missed; j->drops+=missed*fragments((uint16_t)(4u+s->tofZones*4u));
                j->nextSeq=(uint16_t)(j->nextSeq+missed);
            }
            sampleTof(j,&s->tofStream,in);
        }
    }
    budget=4u;
    while(budget && (s->imuStream.len || s->tofStream.len)) {
        DT_Stream *p=s->imuStream.len?&s->imuStream:&s->tofStream;
        if(!emitFragment(emit,ctx,DT_DATA_CID,s->session,DT_u16(j->cache.requestBytes+8),
                         p->seq,p->stamp,p->kind,DT_OK,p->body,p->len,p->index)) {
            finish(s,in,DT_OVERFLOW); return;
        }
        budget--;
        if(++p->index==fragments(p->len)) p->len=0u;
    }
    if(in->nowMs-j->start>=j->duration && !s->imuStream.len && !s->tofStream.len) {
        finish(s,in,DT_OK); return;
    }
    if(in->nowMs-j->progressMs>=500u && s->ctrlCount<DT_CONTROL_QUEUE-1u) {
        DT_Record r;
        j->progressMs=in->nowMs;
        record(&r,s->session,DT_u16(j->cache.requestBytes+8),j->nextSeq++,DT_PROGRESS,DT_OK,in->nowMs);
        t8(&r,9u,DT_RUNNING); t32(&r,14u,in->nowMs-j->start); enqueue(s,&r);
    }
}
