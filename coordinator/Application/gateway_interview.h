#ifndef GATEWAY_INTERVIEW_H
#define GATEWAY_INTERVIEW_H
#include "control_sync.h"
/* Included after sendZcl. Bounded, task-context ZDO discovery; no fixed node EP. */
static void remember(const uint8_t *ieee, uint16_t address)
{
    int freeSlot = -1;
    /* Short addresses can be reassigned. Never route a new node's traffic
     * through an old IEEE/endpoint cache entry with the same short address. */
    for (unsigned i=0; i<CHILD_SLOTS; ++i)
        if (children[i].used && children[i].address==address &&
            memcmp(children[i].ieee,ieee,8))
            memset(&children[i],0,sizeof(children[i]));
    for (unsigned i=0; i<CHILD_SLOTS; ++i) {
        if (children[i].used && !memcmp(children[i].ieee, ieee, 8)) {
            if(children[i].address!=address) {
                children[i].stage=0; children[i].endpoint=0; children[i].hy=0;
                children[i].modelReady=0;children[i].modelAt=0;
            }
            children[i].address=address; return;
        }
        if (!children[i].used && freeSlot < 0) freeSlot=i;
    }
    if (freeSlot >= 0) { children[freeSlot].used=1; children[freeSlot].address=address;
        memcpy(children[freeSlot].ieee, ieee, 8); }
}
static int childIndex(uint16_t address)
{
    for(unsigned i=0;i<CHILD_SLOTS;i++)
        if(children[i].used && children[i].address==address) return (int)i;
    return -1;
}
static void interviewState(unsigned i,uint8_t state)
{
    uint8_t p[12]; memcpy(p,children[i].ieee,8); Gateway_Put16(p+8,children[i].address);
    p[10]=state; p[11]=children[i].endpoint; emit(0xA2,p,sizeof(p));
}
static void interviewStart(unsigned i)
{
    if((children[i].stage>=1 && children[i].stage<=3) || children[i].stage==6) return;
    children[i].stage=1; children[i].retries=0; children[i].hy=0;
    children[i].endpoint=0; children[i].epCount=children[i].epIndex=0;
    children[i].interviewAt=millis(); interviewState(i,1);
}
static void interviewTick(uint32_t now)
{
    for(unsigned i=0;i<CHILD_SLOTS;i++) {
        if(children[i].used && children[i].stage==5 && due(now,children[i].interviewAt))
            interviewStart(i);
        if(children[i].used && children[i].endpoint && !children[i].modelReady &&
           due(now,children[i].modelAt)) {
            uint8_t model[]={5,0};children[i].modelAt=now+8000U;
            (void)sendZcl(children[i].address,BASIC_CLUSTER,0x10,0,model,2,false,-1);
            break;
        }
        if(!children[i].used || !((children[i].stage>=1 && children[i].stage<=3) || children[i].stage==6) ||
           !due(now,children[i].interviewAt)) continue;
        if(children[i].retries++>=3) { children[i].stage=5;children[i].interviewAt=now+15000U; interviewState(i,4); continue; }
        children[i].interviewAt=now+4000;
        if(children[i].stage==1) {
            zstack_zdoActiveEndpointReq_t r={0};
            r.dstAddr=r.nwkAddrOfInterest=children[i].address;
            (void)Zstackapi_ZdoActiveEndpointReq(entity,&r);
        } else if(children[i].stage==2) {
            zstack_zdoSimpleDescReq_t r={0};
            r.dstAddr=r.nwkAddrOfInterest=children[i].address;
            r.endpoint=children[i].eps[children[i].epIndex];
            (void)Zstackapi_ZdoSimpleDescReq(entity,&r);
        } else if(children[i].stage==3) {
            uint8_t attributes[]={0,0,1,0};
            (void)sendZcl(children[i].address,HY_CONTROL_CLUSTER,0x10,0,attributes,4,false,-1);
        } else {
            uint8_t hello[]={HY_PROTOCOL_VERSION,0};
            (void)sendZcl(children[i].address,HY_CONTROL_CLUSTER,0x11,0,hello,2,false,-1);
        }
        break; /* Bound requests per application iteration. */
    }
}
static void interviewEndpoints(const zstack_zdoActiveEndpointsRspInd_t *r)
{
    int i=childIndex(r->srcAddr);
    if(i<0 || children[i].stage!=1 || r->nwkAddrOfInterest!=r->srcAddr) return;
    if(r->status || !r->n_activeEPList || r->n_activeEPList>16 || !r->pActiveEPList) {
        children[i].stage=5; interviewState(i,4); return;
    }
    memcpy(children[i].eps,r->pActiveEPList,r->n_activeEPList);
    children[i].epCount=r->n_activeEPList; children[i].epIndex=0;
    children[i].stage=2; children[i].retries=0; children[i].interviewAt=millis();
}
static void interviewDescriptor(const zstack_zdoSimpleDescRspInd_t *r)
{
    int i=childIndex(r->srcAddr);
    if(i<0 || children[i].stage!=2 || r->nwkAddrOfInterest!=r->srcAddr) return;
    const zstack_SimpleDescriptor_t *d=&r->simpleDesc;
    if(r->status) { children[i].stage=5; interviewState(i,4); return; }
    if(d->endpoint!=children[i].eps[children[i].epIndex]) return;
    if(d->n_inputClusters>32 || d->n_outputClusters>32 ||
       (d->n_inputClusters && !d->pInputClusters) || (d->n_outputClusters && !d->pOutputClusters)) {
        children[i].stage=5; interviewState(i,4); return;
    }
    uint8_t p[144]; memcpy(p,children[i].ieee,8); Gateway_Put16(p+8,r->srcAddr);
    p[10]=1; p[11]=d->endpoint; Gateway_Put16(p+12,d->profileID);
    p[14]=d->n_inputClusters; p[15]=d->n_outputClusters; unsigned n=16,flags=0;
    for(unsigned k=0;k<d->n_inputClusters;k++) {
        uint16_t c=d->pInputClusters[k]; Gateway_Put16(p+n,c); n+=2;
        if(c==HY_CONTROL_CLUSTER)flags|=1; if(c==HY_DATA_CLUSTER)flags|=2;
        if(c==BASIC_CLUSTER)flags|=4;
    }
    for(unsigned k=0;k<d->n_outputClusters;k++) { Gateway_Put16(p+n,d->pOutputClusters[k]); n+=2; }
    emit(0xA2,p,(uint8_t)n);
    if(d->profileID==0x0104 && (flags&3)==3) {
        children[i].endpoint=d->endpoint; children[i].hy=1;
    } else if(!children[i].endpoint && (flags&4)) children[i].endpoint=d->endpoint;
    if(++children[i].epIndex<children[i].epCount) {
        children[i].retries=0; children[i].interviewAt=millis(); return;
    }
    uint8_t model[]={5,0};
    if(children[i].endpoint) (void)sendZcl(r->srcAddr,BASIC_CLUSTER,0x10,0,model,2,false,-1);
    children[i].stage=children[i].hy?3:4;
    children[i].retries=0; children[i].interviewAt=millis();
    if(!children[i].hy) interviewState(i,2);
}
static void interviewProtocol(uint16_t addr,uint8_t ep,const uint8_t *p,unsigned len)
{
    int i=childIndex(addr);
    if(i<0 || children[i].stage!=3 || children[i].endpoint!=ep) return;
    /* Read Attributes response for protocol(uint16) and capabilities(bitmap16). */
    if(len!=12 || Gateway_LE16(p)!=0 || p[2] || p[3]!=0x21 ||
       Gateway_LE16(p+6)!=1 || p[8] || p[9]!=0x19) return;
    uint8_t out[14]; memcpy(out,children[i].ieee,8); Gateway_Put16(out+8,addr);
    memcpy(out+10,p+4,2); memcpy(out+12,p+10,2); emit(0xA3,out,sizeof(out));
    if(Gateway_LE16(p+4)!=HY_PROTOCOL_VERSION) {
        children[i].stage=5; interviewState(i,4); return;
    }
    children[i].hy=2; children[i].stage=6;
    children[i].retries=0; children[i].interviewAt=millis();
    /* Wait for an uplink after HELLO; AF submission alone is not readiness. */
}
static void interviewUplink(unsigned i,uint8_t cmd,unsigned size)
{
    if(((cmd==0x99 && size==1) || (cmd==CONTROL_STATUS_CMD && size==CONTROL_STATUS_SIZE)) &&
       children[i].hy==2 && (children[i].stage==6 || children[i].stage==5)) {
        children[i].stage=4; interviewState(i,3);
    }
}
#endif
