#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "gateway_protocol.h"
#include "radio_diag_protocol.h"
#include "sensor_clusters.h"
#include "comm_test_protocol.h"
#include "scalar_delivery.h"
#include "upload_trace.h"
#include "window_reply_coalesce.h"
#define GW_EP 8
#define BASIC_CLUSTER 0
#define zstack_AFAddrMode_SHORT 2
#define UART2_STATUS_SUCCESS 0
typedef struct {
    uint8_t endpoint;struct{uint8_t endpoint,addrMode;union{uint16_t shortAddr;} addr;}srcAddr;
    uint16_t clusterId,n_payload;uint8_t *pPayload;int8_t rssi;uint8_t linkQuality;
} zstack_afIncomingMsgInd_t;
static uint32_t clock_ms;static uint32_t millis(void){return clock_ms;}
static uint8_t uart=1,scanning;
static bool windowQueryTagged;
static struct{uint8_t hy,endpoint,modelReady;}children[1]={{2,1,0}};
static uint8_t writes[128][259];static unsigned sizes[128],written_count;
static int fail_write,filter_index;static unsigned replies,interviews;
static int_fast16_t UART2_write(uint8_t handle,const void *data,size_t length,size_t *written){
    assert(handle==1 && written_count<128 && length<=259);
    memcpy(writes[written_count],data,length);sizes[written_count++]=(unsigned)length;
    *written=fail_write?length-1:length;return UART2_STATUS_SUCCESS;
}
static bool emitChecked(uint8_t,const uint8_t *,uint8_t);
static void emit(uint8_t,const uint8_t *,uint8_t);
#include "upload_trace_coordinator.h"
static int childIndex(uint16_t address){assert(address==0x714C);return filter_index;}
static void interviewProtocol(uint16_t a,uint8_t ep,const uint8_t *p,unsigned n){(void)a;(void)ep;(void)p;(void)n;}
static void interviewUplink(unsigned i,uint8_t c,unsigned n){(void)i;(void)c;(void)n;interviews++;}
static bool lookup(uint16_t a,uint8_t *ieee){(void)a;memset(ieee,0,8);return true;}
static void controlApplied(unsigned i,const uint8_t *p,unsigned n){(void)i;(void)p;(void)n;}
static unsigned diag_replies;static uint8_t diag_echo[7],diag_submit_result;
static uint8_t nodeCommand(uint16_t a,uint8_t c,const uint8_t *p,uint8_t n,bool t){(void)a;
    if(c==COMM_TEST_REPLY){assert(n==7 && !t);memcpy(diag_echo,p,7);diag_replies++;return diag_submit_result;}return 0;}
static uint8_t sendZcl(uint16_t a,uint16_t cl,uint8_t fc,uint8_t cmd,const uint8_t *p,uint8_t n,bool t,int seq){
    (void)a;(void)cl;(void)fc;(void)cmd;(void)p;(void)n;(void)t;(void)seq;replies++;return 0;
}
static void incoming(const zstack_afIncomingMsgInd_t *);
int main(void){
    uint8_t p[82]={0x19,255,0x94,42,0,0,0,11,0,19};
    zstack_afIncomingMsgInd_t m={0};m.endpoint=8;m.srcAddr.endpoint=1;m.srcAddr.addrMode=2;
    m.srcAddr.addr.shortAddr=0x714C;m.clusterId=HY_DATA_CLUSTER;m.n_payload=82;m.pPayload=p;m.rssi=-47;m.linkQuality=150;
    incoming(&m);assert(written_count==1 && interviews==1 && !replies && uploadTraceHistory.count==2);
    assert(sizes[0]==85 && writes[0][0]==0xFE && writes[0][1]==0x94 && writes[0][2]==81);
    assert(ut_u16(writes[0]+3)==0x714C && !memcmp(writes[0]+5,p+3,79));
    assert(uploadTraceHistory.records[0].p[1]==UT_COORD_RX && uploadTraceHistory.records[1].p[1]==UT_COORD_FORWARD);
    assert(uploadTraceHistory.records[0].p[9]==255);
    assert(ut_u32(uploadTraceHistory.records[0].p+20)==ut_fingerprint(p+3,79));
    /* Filter failure remains a drop; it is now observable before forwarding. */
    filter_index=-1;clock_ms=100;incoming(&m);assert(interviews==1 && uploadTraceHistory.count==4);
    assert(uploadTraceHistory.records[3].p[1]==UT_COORD_DROP && uploadTraceHistory.records[3].p[11]==1);
    /* A short UART write is recorded separately from radio receipt. */
    filter_index=0;fail_write=1;clock_ms=11000;incoming(&m);assert(interviews==2);
    assert(uploadTraceHistory.records[5].p[1]==UT_COORD_FORWARD && uploadTraceHistory.records[5].p[11]==4);
    RadioAckMeta meta={0};uploadTraceAf(RADIO_DIAG_SUBMIT,0x714C,1,2,&meta,0,0);
    assert(uploadTraceHistory.records[6].p[1]==UT_COORD_SUBMIT);
    /* An actual token-echo frame includes both measurements from that frame,
     * retaining the legacy RSSI byte offset. */
    scanning=1;fail_write=0;p[0]=0x19;p[2]=0x9B;p[3]=0x12;p[4]=0x34;m.n_payload=5;m.clusterId=HY_CONTROL_CLUSTER;
    unsigned before=written_count;incoming(&m);
    assert(written_count==before+1 && writes[before][1]==0x9B && writes[before][2]==6);
    assert(ut_u16(writes[before]+3)==0x714C && writes[before][5]==0x12 && writes[before][6]==0x34);
    assert((int8_t)writes[before][7]==-47 && writes[before][8]==150);
    /* Serial diagnostics work with admission closed and do not mark the node
     * interviewed, authorized or online. */
    scanning=0;p[2]=COMM_TEST_REQUEST;comm_test_request(p+3,123456U);m.n_payload=8;
    before=written_count;unsigned previous_interviews=interviews;incoming(&m);
    assert(diag_replies==1 && interviews==previous_interviews && written_count==before+1);
    assert(comm_test_token(diag_echo)==123456U && (int8_t)diag_echo[5]==-47 && diag_echo[6]==150);
    assert(writes[before][1]==COMM_TEST_REPORT && writes[before][2]==10);
    assert(comm_test_token(writes[before]+5)==123456U && !writes[before][12]);
    before=written_count;diag_submit_result=0x10;incoming(&m);
    assert(writes[before][12]==0x10 && interviews==previous_interviews);
    before=written_count;p[3]=2;incoming(&m);assert(written_count==before && diag_replies==2);
    /* A legacy UART peer gets the original frame. Opt-in appends the exact
     * uplink sequence without changing the on-air or trace payload. */
    const uint8_t query[]={0x19,35,0x95,2,3,42,0,0,0,12,0};
    memcpy(p,query,sizeof(query));m.n_payload=sizeof(query);m.clusterId=HY_DATA_CLUSTER;
    before=written_count;incoming(&m);
    assert(written_count==before+1 && writes[before][2]==10);
    windowQueryTagged=true;
    before=written_count;incoming(&m);
    assert(written_count==before+1 && writes[before][2]==11);
    assert(!memcmp(writes[before]+5,query+3,8) && writes[before][13]==35);
    unsigned pos=(uploadTraceHistory.head+UT_RING_SIZE-1U)%UT_RING_SIZE;
    assert(ut_u32(uploadTraceHistory.records[pos].p+20)==ut_fingerprint(query+3,8));
    p[4]=4;before=written_count;incoming(&m);
    assert(written_count==before+1 && writes[before][2]==10);

    uint8_t wire[20]={0x4c,0x71,2,3,42,0,0,0,12,0,1};
    uint8_t ieee[8]={1},other[8]={2};
    WindowReplyTag pending={.valid=true,.query_seq=35,.ieee={1}};
    memcpy(pending.payload,wire+2,WINDOW_REPLY_LEN);
    assert(window_reply_has_tag(wire,sizeof(wire)));
    assert(!window_reply_has_tag(wire,sizeof(wire)-1U));
    assert(window_reply_matches(&pending,ieee,35,wire+2,1451U));
    assert(!window_reply_matches(&pending,ieee,36,wire+2,1451U));
    assert(!window_reply_matches(&pending,other,35,wire+2,1451U));
    assert(!window_reply_matches(&pending,ieee,35,wire+2,3000U));
    wire[18]=1; /* A changed missing-fragment bitmap needs a fresh reply. */
    assert(!window_reply_matches(&pending,ieee,35,wire+2,1451U));
    wire[18]=0;pending.valid=false;
    assert(!window_reply_matches(&pending,ieee,35,wire+2,1451U));
    puts("Coordinator trace and window query PASS");
}
