#ifndef GATEWAY_RADIO_DIAG_H
#define GATEWAY_RADIO_DIAG_H
#include "radio_diag_protocol.h"
/* Included after millis/emit. MAC callbacks only copy bounded metadata.
 * Never enable appPendingQueue: that changes normal NWK/MAC queue ownership. */
#ifndef macCfg
extern macCfg_t macCfg;
#endif
#define RADIO_RING_SIZE 64U
typedef struct {
    uint32_t tick;uint16_t address,options;
    uint8_t type,status,handle,retries,flags,noRsp;int8_t rssi;
} RadioRaw;
static RadioRaw radioRing[RADIO_RING_SIZE];
static volatile unsigned radioHead,radioTail;
static volatile uint32_t radioDropped;
static volatile uint8_t radioDiagEnabled;
static uint8_t radioInstalled;
static uint8_t (*radioPreviousCallback)(uint8_t *);
static struct {uint32_t count,tick;uint16_t address;uint8_t used;} radioPolls[64];
static void radioDiagPush(const RadioRaw *r)
{
    UInt key=Hwi_disable();unsigned next=(radioHead+1U)%RADIO_RING_SIZE;
    if(!radioDiagEnabled){Hwi_restore(key);return;}
    if(next==radioTail) {if(radioDropped!=UINT32_MAX)radioDropped++;}
    else {radioRing[radioHead]=*r;radioHead=next;}
    Hwi_restore(key);
}
static uint8_t radioDiagObserve(uint8_t *message)
{
    if(radioDiagEnabled && message) {
        const macCbackEvent_t *e=(const macCbackEvent_t *)message;
        RadioRaw r={0};r.tick=millis();
        if(e->hdr.event==MAC_MLME_POLL_IND && e->pollInd.srcAddr.addrMode==SADDR_MODE_SHORT) {
            r.type=RADIO_DIAG_POLL;r.address=e->pollInd.srcAddr.addr.shortAddr;r.noRsp=e->pollInd.noRsp;
            r.status=e->hdr.status;radioDiagPush(&r);
        } else if(e->hdr.event==MAC_MCPS_DATA_CNF && e->dataCnf.pDataReq &&
                  e->dataCnf.pDataReq->mac.dstAddr.addrMode==SADDR_MODE_SHORT) {
            r.type=RADIO_DIAG_MAC_RESULT;r.address=e->dataCnf.pDataReq->mac.dstAddr.addr.shortAddr;
            r.status=e->hdr.status;r.handle=e->dataCnf.msduHandle;r.retries=e->dataCnf.retries;
            r.options=e->dataCnf.pDataReq->internal.txOptions;r.flags=RADIO_DIAG_HAS_REQ;
            if(r.options&MAC_TXOPTION_INDIRECT)r.flags|=RADIO_DIAG_INDIRECT;
            if(!r.status && (r.options&MAC_TXOPTION_ACK)) {r.flags|=RADIO_DIAG_MAC_ACK;r.rssi=e->dataCnf.rssi;}
            radioDiagPush(&r);
        }
    }
    /* Preserve any existing observer/filter and its ownership decision. */
    return radioPreviousCallback ? radioPreviousCallback(message) : FALSE;
}
static void radioDiagPollSnapshot(uint8_t *p,uint16_t address,uint32_t now)
{
    for(unsigned i=0;i<64;i++)if(radioPolls[i].used && radioPolls[i].address==address) {
        radio_w32(p+24,radioPolls[i].count);radio_w32(p+28,now-radioPolls[i].tick);return;
    }
}
static void radioDiagDrain(void)
{
    if(!radioDiagEnabled)return;
    for(unsigned n=0;n<RADIO_RING_SIZE;n++) {
        UInt key=Hwi_disable();
        if(radioTail==radioHead){Hwi_restore(key);break;}
        RadioRaw r=radioRing[radioTail];radioTail=(radioTail+1U)%RADIO_RING_SIZE;Hwi_restore(key);
        if(r.type==RADIO_DIAG_POLL) {
            unsigned slot=0;uint32_t oldest=0;
            for(unsigned i=0;i<64;i++) {
                if(radioPolls[i].used && radioPolls[i].address==r.address){slot=i;break;}
                if(!radioPolls[i].used){slot=i;break;}
                if(r.tick-radioPolls[i].tick>=oldest){oldest=r.tick-radioPolls[i].tick;slot=i;}
            }
            if(!radioPolls[slot].used || radioPolls[slot].address!=r.address) {
                memset(&radioPolls[slot],0,sizeof(radioPolls[slot]));
                radioPolls[slot].used=1;radioPolls[slot].address=r.address;
            }
            if(radioPolls[slot].count!=UINT32_MAX)radioPolls[slot].count++;
            radioPolls[slot].tick=r.tick;
        }
        uint8_t p[RADIO_DIAG_LEN];radio_diag_base(p,r.type,r.tick,r.address);
        p[18]=r.status;p[19]=r.flags;radio_w16(p+32,r.options);p[34]=r.handle;
        p[35]=r.retries;p[36]=(uint8_t)r.rssi;p[37]=r.noRsp;
        radioDiagPollSnapshot(p,r.address,r.tick);emit(0x9F,p,sizeof(p));
    }
    UInt key=Hwi_disable();uint32_t lost=radioDropped;radioDropped=0;Hwi_restore(key);
    if(lost){uint8_t p[RADIO_DIAG_LEN];radio_diag_base(p,RADIO_DIAG_DROPPED,millis(),0xFFFF);
        radio_w32(p+24,lost);emit(0x9F,p,sizeof(p));}
}
static void radioDiagConfigure(uint8_t enabled)
{
    UInt key=Hwi_disable();
    if(enabled && !radioInstalled) {
        radioPreviousCallback=pZMac_AppCallback;pZMac_AppCallback=radioDiagObserve;radioInstalled=1;
    }
    radioDiagEnabled=enabled;
    if(!enabled){radioTail=radioHead;radioDropped=0;}
    Hwi_restore(key);
    if(enabled) {
        uint8_t p[RADIO_DIAG_LEN];radio_diag_base(p,RADIO_DIAG_COVERAGE,millis(),0xFFFF);
        p[19]=(macCfg.appPendingQueue?1U:0U)|(pZMac_AppCallback==radioDiagObserve?2U:0U);
        emit(0x9F,p,sizeof(p));
    }
}
static void radioDiagAf(uint8_t type,uint16_t address,uint8_t id,uint8_t seq,
                        const RadioAckMeta *m,uint8_t status,uint32_t elapsed)
{
    uploadTraceAf(type,address,id,seq,m,status,elapsed);
    /* Upload reply failures carry their identity even without verbose MAC
     * observation. Successes and poll traffic remain opt-in. */
    if(!radioDiagEnabled && !(status &&
       (type==RADIO_DIAG_SUBMIT || type==RADIO_DIAG_AF_RESULT)))return;
    radioDiagDrain();uint32_t now=millis();uint8_t p[RADIO_DIAG_LEN];
    radio_diag_base(p,type,now,address);p[8]=id;p[9]=seq;p[10]=m->kind;p[11]=m->result;
    if(type==RADIO_DIAG_AF_RESULT || (type==RADIO_DIAG_SUBMIT && !status))p[19]|=RADIO_DIAG_AF_ID_VALID;
    radio_w32(p+12,m->batch);radio_w16(p+16,m->block);p[18]=status;radio_w32(p+20,elapsed);
    radioDiagPollSnapshot(p,address,now);emit(0x9F,p,sizeof(p));
}
#endif
