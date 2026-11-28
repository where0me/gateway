#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "radio_diag.h"
typedef unsigned UInt;
#define FALSE 0
#define SADDR_MODE_SHORT 2
#define MAC_MLME_POLL_IND 16
#define MAC_MCPS_DATA_CNF 12
#define MAC_TXOPTION_ACK 1
#define MAC_TXOPTION_INDIRECT 4
typedef struct {uint8_t addrMode;union{uint16_t shortAddr;} addr;} TestAddr;
typedef struct {uint8_t event,status;} TestHdr;
typedef struct {struct{TestAddr dstAddr;} mac;struct{uint16_t txOptions;} internal;} macMcpsDataReq_t;
typedef union {
    TestHdr hdr;
    struct{TestHdr hdr;TestAddr srcAddr;uint8_t noRsp;} pollInd;
    struct{TestHdr hdr;macMcpsDataReq_t *pDataReq;uint8_t msduHandle,retries;int8_t rssi;} dataCnf;
} macCbackEvent_t;
typedef struct {bool appPendingQueue;} macCfg_t;
macCfg_t macCfg;
static uint8_t (*pZMac_AppCallback)(uint8_t *);
static uint32_t clock_ms;
static unsigned lock_depth,emitted,previous_calls,trace_emitted;
static uint8_t previous_result;
static uint8_t emitted_frames[1024][RADIO_DIAG_LEN];
static uint32_t millis(void){return clock_ms;}
static UInt Hwi_disable(void){return lock_depth++;}
static void Hwi_restore(UInt key){assert(lock_depth && key==lock_depth-1);lock_depth=key;}
static uint8_t previous(uint8_t *p){(void)p;assert(!lock_depth);previous_calls++;return previous_result;}
static void emit(uint8_t cmd,const uint8_t *p,uint8_t len)
{assert(!lock_depth && cmd==0x9F && len==RADIO_DIAG_LEN && emitted<1024);if(p[0]==3){trace_emitted++;return;}memcpy(emitted_frames[emitted++],p,len);}
#include "upload_trace_coordinator.h"
#include "gateway_radio_diag.h"

/* The runner appends the actual bridge sendZcl function. */
#define GW_EP 8
#define TX_SLOTS 32
#define CHILD_SLOTS 64
static uint8_t ready=1,sequence,transaction,entity;
static int controlSendingChild=-1;
static struct {uint8_t used,id,cmd,seq;uint16_t address;uint32_t at,controlGeneration;
    int controlChild;RadioAckMeta radio;} pending[TX_SLOTS];
static struct {uint32_t generation;} controlRetries[CHILD_SLOTS];
static struct {uint8_t used,endpoint;uint16_t address;} children[CHILD_SLOTS];
typedef struct {
    struct {uint8_t addrMode,endpoint;struct{uint16_t shortAddr;} addr;} dstAddr;
    uint8_t srcEndpoint,*transID,radius;uint16_t clusterID,n_payload;
    struct {bool ackRequest;} options;uint8_t *pPayload;
} zstack_afDataReq_t;
#define zstack_AFAddrMode_SHORT 2
static uint8_t af_submit_status;
static unsigned af_submissions;
static uint8_t af_id;
static uint8_t af_bytes[258];static unsigned af_length;
static uint8_t Zstackapi_AfDataReq(uint8_t id,zstack_afDataReq_t *r)
{
    (void)id;assert(r->options.ackRequest && r->dstAddr.addr.shortAddr==0xB905);
    af_length=r->n_payload;memcpy(af_bytes,r->pPayload,af_length);
    af_submissions++;af_id=*r->transID;if(!af_submit_status)(*r->transID)++;return af_submit_status;
}
static uint8_t sendZcl(uint16_t,uint16_t,uint8_t,uint8_t,const uint8_t *,uint8_t,bool,int);
static void reset(void)
{
    radioDiagConfigure(0);memset(radioPolls,0,sizeof(radioPolls));
    radioHead=radioTail=radioDropped=0;clock_ms=0;emitted=previous_calls=0;
    previous_result=0;macCfg.appPendingQueue=false;
}
static void poll(uint16_t address,uint8_t noRsp)
{
    macCbackEvent_t e={0};e.pollInd.hdr.event=MAC_MLME_POLL_IND;
    e.pollInd.srcAddr.addrMode=SADDR_MODE_SHORT;e.pollInd.srcAddr.addr.shortAddr=address;e.pollInd.noRsp=noRsp;
    unsigned before=emitted;assert(pZMac_AppCallback((uint8_t *)&e)==previous_result);
    assert(emitted==before); /* no UART/formatting in MAC callback */
}
int main(void)
{
    uploadTraceIncoming(UT_COORD_RX,0xB905,0x94,0,(const uint8_t[7]){1,0,0,0,0,0,0},7,0,-47,150);
    pZMac_AppCallback=previous;radioDiagConfigure(1);
    assert(pZMac_AppCallback==radioDiagObserve && radioPreviousCallback==previous && !macCfg.appPendingQueue);
    assert(emitted==1 && emitted_frames[0][1]==RADIO_DIAG_COVERAGE && emitted_frames[0][19]==2);
    unsigned before=emitted;radioDiagConfigure(1);
    assert(emitted==before+1 && radioPreviousCallback==previous); // reconnect capability handshake
    char text[1200];RadioDiagLog log;
    assert(radio_diag_format(emitted_frames[0],40,text,sizeof(text),&log));
    assert(strstr(text,"覆盖不完整") && strstr(text,"未修改"));
    reset();poll(0xB905,0);radioDiagDrain();assert(!emitted && previous_calls==1);
    radioDiagConfigure(1);emitted=0;
    clock_ms=250;poll(0xB905,1);clock_ms=500;poll(0xB905,0);
    assert(!emitted);radioDiagDrain();assert(emitted==2);
    assert(radio_u32(emitted_frames[0]+24)==1 && radio_u32(emitted_frames[1]+24)==2);
    assert(emitted_frames[0][37]==1 && emitted_frames[1][37]==0);
    assert(radio_diag_format(emitted_frames[1],40,text,sizeof(text),&log) && strstr(text,"收到MAC取包事件"));
    previous_result=1;poll(0xB905,0);assert(previous_calls==4);radioDiagDrain();previous_result=0;

    macMcpsDataReq_t req={0};req.mac.dstAddr.addrMode=SADDR_MODE_SHORT;
    req.mac.dstAddr.addr.shortAddr=0xB905;req.internal.txOptions=MAC_TXOPTION_ACK|MAC_TXOPTION_INDIRECT;
    macCbackEvent_t cnf={0};cnf.dataCnf.hdr.event=MAC_MCPS_DATA_CNF;cnf.dataCnf.pDataReq=&req;
    cnf.dataCnf.msduHandle=19;cnf.dataCnf.retries=2;cnf.dataCnf.rssi=-47;
    before=emitted;pZMac_AppCallback((uint8_t *)&cnf);assert(emitted==before);
    /* Source objects can be destroyed immediately after the hook returns. */
    memset(&req,0,sizeof(req));memset(&cnf,0,sizeof(cnf));radioDiagDrain();
    assert(emitted==before+1);uint8_t *p=emitted_frames[before];
    assert(p[19]==(RADIO_DIAG_HAS_REQ|RADIO_DIAG_MAC_ACK|RADIO_DIAG_INDIRECT));
    assert(p[34]==19 && p[35]==2 && p[36]==(uint8_t)-47);
    assert(radio_diag_format(p,40,text,sizeof(text),&log) && strstr(text,"已确认") && strstr(text,"-47 dBm"));
    p[18]=0xF0;p[19]&=(uint8_t)~RADIO_DIAG_MAC_ACK;
    assert(radio_diag_format(p,40,text,sizeof(text),&log) && log.persist && !strcmp(log.level,"WARN"));
    assert(strstr(text,"过期或被清理") && strstr(text,"ACK RSSI=不可用") && strstr(text,"不是AF事务号"));
    p[18]=0xE9;assert(radio_diag_format(p,40,text,sizeof(text),&log) && strstr(text,"MAC未收到确认"));

    uint8_t ack[17]={2,3};radio_w32(ack+2,2850209704U);radio_w16(ack+6,25);ack[8]=1;
    RadioAckMeta meta=radio_ack_meta(ack,sizeof(ack));
    assert(meta.batch==2850209704U && meta.block==25 && meta.kind==3 && meta.result==1);
    assert(!radio_ack_meta(ack,8).kind);
    memset(pending,0,sizeof(pending));children[0].used=1;children[0].endpoint=1;children[0].address=0xB905;
    transaction=sequence=255;before=emitted;clock_ms=700;
    assert(!sendZcl(0xB905,0xFC11,0x11,0x0D,ack,sizeof(ack),false,-1));
    assert(af_submissions==1 && af_id==255 && pending[0].id==255 && transaction==0 && pending[0].radio.batch==meta.batch);
    assert(emitted==before+1);p=emitted_frames[before];
    assert((p[19]&RADIO_DIAG_AF_ID_VALID) && p[8]==255 && p[9]==255);
    assert(radio_diag_format(p,40,text,sizeof(text),&log) && strstr(text,"窗口=26") && strstr(text,"AF事务=255"));
    assert(strstr(text,"缺片补传") && strstr(text,"不代表已发射"));
    uint8_t original_bytes[258];memcpy(original_bytes,af_bytes,af_length);unsigned original_length=af_length;
    radioDiagConfigure(0);before=emitted;memset(pending,0,sizeof(pending));transaction=sequence=255;
    assert(!sendZcl(0xB905,0xFC11,0x11,0x0D,ack,sizeof(ack),false,-1));
    assert(emitted==before && af_length==original_length && !memcmp(original_bytes,af_bytes,af_length));
    assert(pending[0].id==255);radioDiagConfigure(1);
    memset(ack,0,sizeof(ack));assert(pending[0].radio.batch==2850209704U);
    clock_ms=742;before=emitted;radioDiagAf(RADIO_DIAG_AF_RESULT,0xB905,255,255,&pending[0].radio,0xB7,42);
    assert(emitted==before+1 && pending[0].used); // observer cannot retire AF tracking
    assert(radio_diag_format(emitted_frames[before],40,text,sizeof(text),&log) && strstr(text,"APS未收到确认") && log.persist);
    before=emitted;radioDiagAf(RADIO_DIAG_AF_RESULT,0x2222,5,6,&meta,0xF0,99);
    assert(radio_diag_format(emitted_frames[before],40,text,sizeof(text),&log) && strstr(text,"未观测（不等于未轮询）"));

    /* Ordinary mode retains exact failure identity without enabling MAC
     * callbacks or flooding successful submissions/results. */
    radioDiagConfigure(0);before=emitted;
    radioDiagAf(RADIO_DIAG_SUBMIT,0xB905,5,6,&meta,0,0);
    radioDiagAf(RADIO_DIAG_AF_RESULT,0xB905,5,6,&meta,0,190);
    assert(emitted==before);
    const char *kinds[]={"批次许可","窗口描述确认","窗口校验结果","整批波形确认",
        "振动特征确认","传感器数据校验","上传许可续期","批次已完成"};
    for(unsigned kind=1;kind<=8;kind++) {
        meta.kind=(uint8_t)kind;before=emitted;
        radioDiagAf(RADIO_DIAG_AF_RESULT,0xB905,5,6,&meta,0xF0,190);
        assert(emitted==before+1);
        assert(radio_diag_format(emitted_frames[before],40,text,sizeof(text),&log));
        char label[32];snprintf(label,sizeof(label),"编号%u",kind);
        assert(strstr(text,kinds[kind-1]) && strstr(text,label));
        assert(strstr(text,"发送结果失败") && strstr(text,"批次=2850209704") && strstr(text,"耗时=190 ms"));
        assert((kind==2 || kind==3)?strstr(text,"窗口=26")!=NULL:strstr(text,"窗口=-")!=NULL);
    }
    meta.kind=3;before=emitted;
    radioDiagAf(RADIO_DIAG_SUBMIT,0xB905,255,255,&meta,0x10,0);
    assert(emitted==before+1);
    assert(radio_diag_format(emitted_frames[before],40,text,sizeof(text),&log));
    assert(strstr(text,"提交失败") && strstr(text,"AF事务=未知") && strstr(text,"编号3"));
    meta.kind=99;before=emitted;
    radioDiagAf(RADIO_DIAG_AF_RESULT,0xB905,5,6,&meta,0xF0,190);
    assert(radio_diag_format(emitted_frames[before],40,text,sizeof(text),&log));
    assert(strstr(text,"未知回复（编号99）"));

    /* Overload drops diagnostic copies only, preserving every callback. */
    reset();radioDiagConfigure(1);emitted=previous_calls=0;
    for(unsigned i=0;i<100;i++){clock_ms=i;poll(0xB905,0);}
    assert(previous_calls==100 && radioDropped==37 && !emitted);
    radioDiagDrain();assert(emitted==64 && emitted_frames[63][1]==RADIO_DIAG_DROPPED);
    assert(radio_u32(emitted_frames[63]+24)==37 && !radioDropped);
    assert(radio_diag_format(emitted_frames[63],40,text,sizeof(text),&log) && log.persist);
    radioDiagConfigure(0);before=emitted;poll(0xB905,0);radioDiagDrain();assert(emitted==before);
    assert(!macCfg.appPendingQueue);
    /* Hostile/malformed diagnostic records cannot become stored business data. */
    assert(!radio_diag_format(NULL,40,text,sizeof(text),&log));
    p=emitted_frames[0];assert(!radio_diag_format(p,39,text,sizeof(text),&log));
    p[0]=1;assert(!radio_diag_format(p,40,text,sizeof(text),&log));
    p[0]=2;p[1]=99;assert(!radio_diag_format(p,40,text,sizeof(text),&log));
    puts("Radio diagnostics PASS: passive chain/ownership, copied metadata, partial poll coverage, MAC vs APS status, ACK validity, AF255 identity, overflow, disable, no MAC configuration changes");
}
