#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "vibration_tx.h"

static void grant(uint8_t *p,uint32_t sent,uint32_t cookie,uint32_t server,
                  uint32_t start,unsigned lane,unsigned count) {
    memset(p,0,V2_TIMED_ACK_LEN);v2_ack(p,V2_KIND_BLOCK,1,0,V2_TIMED_OK,0);
    v2_w32(p+9,V2_EPOCH_MS);
    v2_w32(p+17,cookie);v2_w32(p+21,sent);v2_w32(p+25,server);
    v2_w32(p+29,start);p[33]=(uint8_t)count;p[34]=(uint8_t)lane;v2_w16(p+35,V2_SLOT_MS);
}
int main(void) {
    uint8_t p[V2_TIMED_ACK_LEN];V2TimedClock a={0},b={0};
    /* Different clock origins and delayed grant delivery still align to one grid. */
    grant(p,100,1,10020,10500,0,2);
    assert(v2_timed_accept(&a,p,140,100,1));
    grant(p,5000,2,10050,10500,1,2);
    assert(v2_timed_accept(&b,p,5040,5000,2));
    unsigned as=0,bs=0;
    for(uint32_t server=10400;server<10500+20U*(2U*V2_SLOT_MS+V2_CONTROL_MS);server++) {
        int x=v2_timed_data_ready(&a,server-a.offset);
        int y=v2_timed_data_ready(&b,server-b.offset);
        assert(!(x && y));as+=x;bs+=y;
        if(x)assert(!v2_timed_data_ready(&a,server-a.offset));
    }
    assert(as==20 && bs==20); /* two 20-frame blocks in 9.6 s of schedule time */
    /* A late timer does not emit a burst to catch up on skipped rounds. */
    assert(!v2_timed_data_ready(&a,16000-a.offset));
    V2TimedClock bad={0};
    assert(!v2_timed_accept(&bad,p,5500,5000,2)); /* uncertain sync */
    assert(!v2_timed_accept(&bad,p,5375,5000,2)); /* too uncertain for short slots */
    assert(!v2_timed_accept(&bad,p,5040,5000,3)); /* old reply */
    assert(!v2_timed_live(&a,140+V2_SYNC_VALID_MS));
    /* Modulo clocks remain valid across uint32 rollover. */
    grant(p,UINT32_MAX-19,7,UINT32_MAX-9,200,0,1);
    V2TimedClock wrap={0};assert(v2_timed_accept(&wrap,p,20,UINT32_MAX-19,7));
    assert(v2_timed_data_ready(&wrap,200+V2_SLOT_OFFSET_MS-wrap.offset));
    assert(!v2_timed_data_ready(&wrap,201+V2_SLOT_OFFSET_MS-wrap.offset));

    V2TimedScheduler s={0};
    unsigned boot_guard=V2_EPOCH_MS+V2_EPOCH_GUARD_MS;
    for(unsigned i=0;i<6;i++)assert(v2_timed_request(&s,i,0)<0);
    for(unsigned i=0;i<6;i++)assert(v2_timed_request(&s,i,boot_guard-1)<0);
    assert(v2_timed_request(&s,0,boot_guard)==0);
    assert(s.count==4);
    assert(v2_timed_request(&s,3,boot_guard)==3);
    assert(v2_timed_request(&s,4,boot_guard)<0);
    v2_timed_forget(&s,1);
    assert(v2_timed_request(&s,4,boot_guard+1000)<0); /* never recycle a live lane */
    for(unsigned i=0;i<6;i++)(void)v2_timed_request(&s,i,s.end-1);
    uint64_t next=s.end+V2_EPOCH_GUARD_MS;
    (void)v2_timed_request(&s,4,next-1);
    (void)v2_timed_request(&s,5,next-1);
    assert(v2_timed_request(&s,4,next)>=0);
    assert(v2_timed_member(&s,5)>=0); /* queued group is not starved */
    /* Startup protection has already elapsed: first ready node gets a grant
       immediately. A late arrival changes membership only after lease expiry. */
    V2TimedScheduler dynamic={0};v2_timed_init(&dynamic,0);
    assert(v2_timed_request(&dynamic,0,30000)==0 && dynamic.count==1);
    uint64_t boundary=dynamic.end+V2_EPOCH_GUARD_MS;
    assert(v2_timed_request(&dynamic,1,31000)<0 && dynamic.count==1);
    assert(v2_timed_request(&dynamic,1,boundary-1)<0);
    assert(v2_timed_request(&dynamic,0,boundary)>=0);
    assert(dynamic.count==2 && v2_timed_member(&dynamic,1)>=0);
    /* An unchanged group renews its lease without shifting the frame grid. */
    V2TimedScheduler stable={0};v2_timed_init(&stable,0);
    assert(v2_timed_request(&stable,0,30000)==0);
    uint64_t origin=stable.start, old_end=stable.end;
    assert(v2_timed_request(&stable,0,40000)==0);
    assert(stable.start==origin && stable.end>old_end && stable.epochs==1);
    assert(v2_timed_request(&stable,1,41000)<0);
    old_end=stable.end;
    assert(v2_timed_request(&stable,0,42000)==0 && stable.end==old_end);
    /* Renewed wire lease is relative to server time, not original start. */
    grant(p,100,3,50000,30500,0,1);
    assert(v2_timed_accept(&bad,p,140,100,3) && bad.end==62000);
    v2_w32(p+9,V2_EPOCH_MS+501U);
    assert(!v2_timed_accept(&bad,p,140,100,3));
    /* Re-synchronization must not reset the block's repair budget. */
    V2Tx tx={0};tx.index=0;tx.fragments=1;v2_w32(tx.batch+1,1);
    for(unsigned i=0;i<=V2_RETRIES;i++) {
        v2_tx_phase(&tx,V2_TX_WINDOW,0);tx.waiting=1;
        v2_ack(p,V2_KIND_WINDOW,1,0,V2_MISSING,1);
        v2_tx_receive(&tx,p,V2_ACK_LEN,1);
    }
    assert(tx.phase==V2_TX_ERROR);
    /* Real hardware log: six slow grants must not kill the first block.
       No uncertain grant is allowed to start streaming; a good sample recovers. */
    memset(&tx,0,sizeof(tx));v2_w32(tx.batch+1,1);tx.fragments=2;
    for(unsigned i=0;i<6;i++) {
        tx.phase=V2_TX_BLOCK;tx.waiting=1;tx.request_sent=1000U*i;
        tx.request_cookie=i+1;
        grant(p,tx.request_sent,tx.request_cookie,10000+1000U*i,10500,0,1);
        v2_tx_receive(&tx,p,sizeof(p),tx.request_sent+373U);
        assert(tx.phase==V2_TX_BLOCK && !tx.waiting && !tx.clock.valid);
        assert(tx.tries==0 && tx.sync_attempts==i+1);
    }
    tx.waiting=1;tx.request_sent=7000;tx.request_cookie=7;
    grant(p,7000,7,17000,10500,0,1);
    v2_tx_receive(&tx,p,sizeof(p),7040);
    assert(tx.phase==V2_TX_STREAM && tx.sync_attempts==0);
    /* Persistent calibration failure remains bounded, including tick wrap. */
    memset(&tx,0,sizeof(tx));v2_w32(tx.batch+1,1);tx.request_sent=UINT32_MAX-100U;
    tx.request_cookie=9;
    grant(p,tx.request_sent,9,10000,10500,0,1);
    for(unsigned i=0;i<V2_SYNC_ATTEMPTS;i++) {
        tx.phase=V2_TX_BLOCK;tx.waiting=1;
        v2_tx_receive(&tx,p,sizeof(p),tx.request_sent+373U);
        assert(tx.phase==(i+1U==V2_SYNC_ATTEMPTS?V2_TX_ERROR:V2_TX_BLOCK));
    }
    puts("timeslots: clock offsets, delayed grants, no duplicate/catch-up, stale replies, wrap, fair epoch rotation PASS");
    return 0;
}
