#ifndef UPLOAD_TRACE_COORDINATOR_H
#define UPLOAD_TRACE_COORDINATOR_H
#include "upload_trace.h"
/* Application-task only. MAC callbacks never format, dump or touch this ring. */
static UploadTraceRing uploadTraceHistory;
static UploadTraceLimit uploadTraceLimits[64];
static uint32_t uploadTraceSnapshot;
static inline void uploadTraceCapability(void)
{
    uint8_t p[UT_LEN]={3,UT_CAPABILITY};ut_w32(p+2,millis());ut_w16(p+6,0xFFFF);
    ut_w32(p+24,UT_RING_SIZE);ut_w16(p+28,UT_DUMP_MAX);ut_w32(p+36,UT_COOLDOWN_MS);emit(0x9F,p,sizeof(p));
}
static void uploadTraceDump(uint16_t address,unsigned reason)
{
    uint32_t now=millis();
    if(!ut_allow(uploadTraceLimits,64,address,now))return;
    uint8_t boundary[UT_LEN]={3,UT_SNAPSHOT};ut_w32(boundary+2,now);ut_w16(boundary+6,address);
    boundary[11]=(uint8_t)reason;ut_w32(boundary+24,++uploadTraceSnapshot);
    ut_w32(boundary+36,uploadTraceHistory.overwritten);emit(0x9F,boundary,sizeof(boundary));
    unsigned matches=0;
    for(unsigned i=0;i<uploadTraceHistory.count;i++){
        unsigned pos=(uploadTraceHistory.head+UT_RING_SIZE-1U-i)%UT_RING_SIZE;
        if(ut_u16(uploadTraceHistory.records[pos].p+6)==address)matches++;
    }
    unsigned skip=matches>UT_DUMP_MAX?matches-UT_DUMP_MAX:0;
    for(unsigned i=0;i<uploadTraceHistory.count;i++){
        unsigned pos=(uploadTraceHistory.head+UT_RING_SIZE-uploadTraceHistory.count+i)%UT_RING_SIZE;
        UploadTraceRecord r=uploadTraceHistory.records[pos];
        if(ut_u16(r.p+6)!=address)continue;
        if(skip){skip--;continue;}
        r.p[19]|=UT_SNAPSHOT_FLAG;emit(0x9F,r.p,UT_LEN);
    }
    boundary[1]=UT_SNAPSHOT_END;emit(0x9F,boundary,sizeof(boundary));
}
static void uploadTraceIncoming(unsigned stage,uint16_t address,uint8_t command,uint8_t seq,
                                const uint8_t *p,size_t n,unsigned result,int rssi,unsigned lqi)
{
    UploadTraceRecord r;
    if(!ut_packet(&r,(uint8_t)stage,millis(),address,command,seq,p,n))return;
    /* RX result retains node state; forward/drop use result for transport/filter. */
    if(stage!=UT_COORD_RX)r.p[11]=(uint8_t)result;
    r.p[30]=(uint8_t)rssi;r.p[31]=(uint8_t)lqi;ut_push(&uploadTraceHistory,&r);
    if((stage==UT_COORD_FORWARD || stage==UT_COORD_DROP) && result)uploadTraceDump(address,5);
}
static void uploadTraceAf(uint8_t type,uint16_t address,uint8_t id,uint8_t seq,
                           const RadioAckMeta *m,uint8_t status,uint32_t elapsed)
{
    UploadTraceRecord r={{0}};r.p[0]=3;
    r.p[1]=type==RADIO_DIAG_SUBMIT?UT_COORD_SUBMIT:UT_COORD_RESULT;
    ut_w32(r.p+2,millis());ut_w16(r.p+6,address);r.p[8]=0x0D;r.p[9]=seq;
    r.p[10]=m->kind;r.p[11]=m->result;ut_w32(r.p+12,m->batch);
    ut_w16(r.p+16,(m->kind==2 || m->kind==3)?m->block:UINT16_MAX);
    r.p[18]=255;r.p[19]=UT_SEQ_VALID;ut_w32(r.p+20,m->fingerprint);
    ut_w32(r.p+24,m->detail);ut_w16(r.p+28,17);r.p[30]=id;r.p[31]=status;
    ut_w32(r.p+36,elapsed);ut_push(&uploadTraceHistory,&r);
    if(status)uploadTraceDump(address,1);
}
#endif
