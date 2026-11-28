#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "vibration_tx.h"

static unsigned sends,block_sends,data_sends;
static int blocked;
static int read_raw(void *context,uint32_t batch,uint32_t first,unsigned count,uint8_t *raw) {
    (void)context;(void)first;assert(batch==9);
    memset(raw,0,count*6);return 1;
}
static int send_packet(void *context,uint8_t command,const uint8_t *p,size_t n) {
    (void)context;(void)p;
    if(blocked)return 0;
    ++sends;
    if(command==V2_BLOCK){assert(n==16);++block_sends;}
    if(command==V2_DATA)++data_sends;
    return 1;
}
static const V2TxIO io={NULL,read_raw,send_packet};
static void begin(V2Tx *t,uint32_t now) {
    uint8_t batch[V2_BATCH_LEN]={0};
    batch[0]=V2_VERSION;v2_w32(batch+1,9);v2_w16(batch+5,26667);
    v2_w32(batch+7,512);v2_w16(batch+11,512);
    sends=block_sends=data_sends=0;blocked=0;
    assert(v2_tx_start(t,batch,now));
    v2_tx_poll(t,&io,now);
    assert(t->phase==V2_TX_BATCH && t->waiting);
}
static void reply(V2Tx *t,unsigned kind,unsigned status,uint64_t value,uint32_t now) {
    uint8_t ack[V2_ACK_LEN];
    v2_ack(ack,kind,9,t->index,status,value);
    v2_tx_receive(t,ack,sizeof(ack),now);
}
int main(void) {
    V2Tx t;
    begin(&t,0);
    reply(&t,V2_KIND_BATCH,V2_OK,0,400);
    v2_tx_poll(&t,&io,400);
    assert(t.phase==V2_TX_BLOCK && t.waiting && block_sends==1);
    reply(&t,V2_KIND_BLOCK,V2_OK,0,1100); /* 700 ms grant, no RTT cutoff */
    assert(t.phase==V2_TX_STREAM && !t.waiting);
    unsigned before=sends;
    v2_tx_poll(&t,&io,1100);
    assert(data_sends==1 && sends==before+1 && !t.waiting);
    v2_tx_poll(&t,&io,1111);assert(sends==before+1);
    v2_tx_poll(&t,&io,1112); /* No per-frame backend ACK needed. */
    assert(data_sends==2 && !t.waiting);

    begin(&t,UINT32_MAX-1000U);
    for(unsigned retry=1;retry<=V2_RETRIES;retry++) {
        uint32_t deadline=t.deadline_ms;
        v2_tx_poll(&t,&io,deadline-1U);assert(sends==retry);
        v2_tx_poll(&t,&io,deadline);assert(sends==retry+1U && t.waiting);
    }
    v2_tx_poll(&t,&io,t.deadline_ms);
    assert(t.phase==V2_TX_ERROR && sends==V2_RETRIES+1U);

    begin(&t,0);
    reply(&t,V2_KIND_BATCH,V2_WAIT,500,100);
    assert(t.deferred && t.next_ms==600 && t.wait_count==1 && !t.tries);
    v2_tx_poll(&t,&io,599);assert(sends==1);
    v2_tx_resume(&t,100); /* HAL clock stopped during receiver-capacity sleep */
    v2_tx_poll(&t,&io,100);assert(sends==2 && !t.deferred);

    begin(&t,0);
    reply(&t,V2_KIND_BATCH,V2_OK,0,100);
    v2_tx_poll(&t,&io,100);
    reply(&t,V2_KIND_BLOCK,V2_OK,0,200);
    assert(t.fragments>=2);
    for(unsigned repair=1;repair<=V2_RETRIES+1U;repair++) {
        t.phase=V2_TX_WINDOW;t.waiting=1;
        reply(&t,V2_KIND_WINDOW,V2_MISSING,UINT64_C(1)<<1,repair*1000U);
        if(repair<=V2_RETRIES) {
            assert(t.phase==V2_TX_STREAM && t.missing==2);
            reply(&t,V2_KIND_WINDOW,V2_MISSING,UINT64_C(1)<<1,repair*1000U);
            assert(t.block_repairs==repair); /* Duplicate bitmap isn't a new failure. */
            unsigned data_before=data_sends;
            v2_tx_poll(&t,&io,repair*1000U);
            assert(data_sends==data_before+1 && t.fragment==2);
        } else assert(t.phase==V2_TX_ERROR);
    }
    begin(&t,0);
    reply(&t,V2_KIND_BATCH,V2_OK,0,100);
    blocked=1;
    v2_tx_poll(&t,&io,100);assert(t.phase==V2_TX_BLOCK && !t.waiting);
    v2_tx_poll(&t,&io,t.deadline_ms);assert(t.phase==V2_TX_ERROR);
    /* Later blocks carry metadata but do not await another grant. */
    begin(&t,0);
    v2_w32(t.batch+7,1024);
    reply(&t,V2_KIND_BATCH,V2_OK,0,100);
    v2_tx_poll(&t,&io,100);
    reply(&t,V2_KIND_BLOCK,V2_OK,0,200);
    reply(&t,V2_KIND_WINDOW,V2_OK,0,300);
    assert(t.index==1 && t.phase==V2_TX_PREPARE);
    v2_tx_poll(&t,&io,300);
    assert(t.phase==V2_TX_STREAM && !t.waiting && block_sends==2);
    uint8_t stale[V2_ACK_LEN];
    v2_ack(stale,V2_KIND_WINDOW,9,0,V2_OK,0);
    v2_tx_receive(&t,stale,sizeof(stale),301);
    assert(t.index==1); /* Duplicate preceding OK never advances twice. */
    t.fragment=t.fragments;
    v2_tx_poll(&t,&io,312);
    assert(t.phase==V2_TX_WINDOW && t.waiting);
    unsigned count=sends;
    v2_tx_poll(&t,&io,t.deadline_ms-1);
    assert(sends==count); /* Normal path sends no block-end query. */
    v2_tx_poll(&t,&io,t.deadline_ms);
    assert(sends==count+1 && t.timeout_count==1); /* Lost reply recovery. */
    puts("single upload: initial grant, pushed block OK, no subsequent grant, stale ACK, timeout-only query, 12 ms pacing, bounded retries/backpressure PASS");
    return 0;
}
