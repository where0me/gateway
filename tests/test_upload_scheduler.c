#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "vibration_scheduler.h"
#include "vibration_tx.h"
static int sends;
static int no_read(void *c,uint32_t b,uint32_t f,unsigned n,uint8_t *p) {
    (void)c;(void)b;(void)f;(void)n;(void)p;return 0;
}
static int count_send(void *c,uint8_t cmd,const uint8_t *p,size_t n) {
    (void)c;(void)cmd;(void)p;(void)n;++sends;return 1;
}
int main(void) {
    V2UploadScheduler s={0};
    assert(!v2_schedule_request(&s,0,0,0));
    assert(!v2_schedule_request(&s,1,0,29999));
    assert(v2_schedule_request(&s,1,0,30000));
    memset(&s,0,sizeof(s));s.initialized=1;
    assert(v2_schedule_request(&s,0,0,0));
    assert(!v2_schedule_request(&s,1,0,1));
    assert(!v2_schedule_request(&s,2,0,2));
    v2_schedule_release(&s,0,0);
    assert(!v2_schedule_request(&s,0,1,3)); /* fast sender cannot jump queue */
    assert(v2_schedule_request(&s,1,0,4));
    v2_schedule_release(&s,0,0); /* duplicate completion cannot release another grant */
    assert(s.owner==2);
    v2_schedule_release(&s,1,0);
    assert(v2_schedule_request(&s,2,0,5));
    v2_schedule_forget(&s,2);
    assert(v2_schedule_request(&s,0,1,6));
    assert(!v2_schedule_request(&s,1,1,29000));
    assert(v2_schedule_request(&s,1,1,30007)); /* crashed owner's lease expires */
    assert(s.expirations==1);
    v2_schedule_release(&s,1,1);
    assert(!v2_schedule_owns(&s,0,1,30008));
    /* An abandoned FIFO head expires, so it cannot stall active requesters. */
    assert(v2_schedule_request(&s,0,2,31000));
    assert(!v2_schedule_request(&s,1,2,31001));
    v2_schedule_release(&s,0,2);
    assert(!v2_schedule_request(&s,2,2,31002));
    assert(v2_schedule_request(&s,2,2,42003));

    V2Tx t={0};uint8_t a[V2_ACK_LEN];
    t.phase=V2_TX_BLOCK;t.index=3;t.pace_ms=V2_PACE_MS;
    v2_w32(t.batch+1,42);
    for(unsigned i=0;i<100;i++) {
        t.waiting=1;t.tries=4;
        v2_ack(a,V2_KIND_BLOCK,42,3,V2_WAIT,500);
        v2_tx_receive(&t,a,sizeof(a),1000);
        assert(t.phase==V2_TX_BLOCK && t.deferred && !t.waiting && !t.tries);
        assert(t.next_ms>=1500 && t.next_ms<=1750);
    }
    assert(t.wait_count==100); /* waiting does not exhaust retries */
    v2_tx_resume(&t,1001);assert(!t.deferred && t.next_ms==1001);
    t.waiting=1;t.fragments=1;
    v2_ack(a,V2_KIND_BLOCK,42,3,V2_OK,V2_GRANT_MS);
    v2_w32(a+13,40);
    v2_tx_receive(&t,a,sizeof(a),2000);
    assert(t.leased && t.phase==V2_TX_STREAM && t.pace_ms==40);
    V2TxIO io={NULL,no_read,count_send};
    v2_tx_poll(&t,&io,22001);
    assert(t.phase==V2_TX_BLOCK && t.waiting && sends==1); /* no data without renewed grant */
    t.waiting=1;t.deadline_ms=25001;t.next_ms=22001;
    v2_tx_poll(&t,&io,25001);
    assert(sends==1 && t.timeout_count==1 && t.next_ms>25001);
    v2_tx_radio_result(&t,0,25001);assert(t.radio_failures==1 && t.pace_ms==160);
    /* Wrong node batch/block and a delayed WAIT cannot alter active streaming. */
    t.phase=V2_TX_STREAM;t.waiting=0;
    v2_tx_receive(&t,a,sizeof(a),26000);assert(t.phase==V2_TX_STREAM);
    t.leased=1;t.lease_until=26000;t.next_ms=26000;t.lease_renewals=V2_RETRIES;
    v2_tx_poll(&t,&io,26001);assert(t.phase==V2_TX_ERROR);
    puts("upload scheduler: FIFO, stale waiters, expired grants, duplicate ACK, STOP2 wake, bounded backoff PASS");
    return 0;
}
