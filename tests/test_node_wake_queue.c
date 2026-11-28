#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "node_wake_plan.h"
#include "sampling_cadence.h"
#include "vibration_tx.h"
int main(void) {
    SamplingCadence c={0};sampling_cadence_start(&c,1000,300000);
    uint32_t queue_at=901000;
    assert(node_wake_causes(10000,1,c.next,1,queue_at)==0);
    assert(node_wake_causes(301000,1,c.next,1,queue_at)==NODE_WAKE_SAMPLE);
    assert(sampling_cadence_due(&c,301000)==1 && c.next==601000);
    assert(queue_at==901000); /* acquisition does not restart queue countdown */
    assert(node_wake_causes(500000,1,c.next,1,500000)==NODE_WAKE_QUEUE);
    assert(node_wake_causes(601000,1,c.next,1,601000)==(NODE_WAKE_SAMPLE|NODE_WAKE_QUEUE));
    assert(node_wake_causes(601000,0,c.next,1,601000)==NODE_WAKE_QUEUE);
    assert(node_wake_causes(20,1,UINT32_MAX-10,1,50)==NODE_WAKE_SAMPLE);
    V2Tx t={0};t.phase=V2_TX_BATCH;t.waiting=1;t.batch[0]=V2_VERSION;v2_w32(t.batch+1,42);
    uint8_t ack[V2_ACK_LEN];v2_ack(ack,V2_KIND_BATCH,42,0,V2_WAIT,600000U);
    v2_tx_receive(&t,ack,sizeof(ack),100);
    assert(t.deferred && t.next_ms==600100 && t.phase==V2_TX_BATCH && !t.tries);
    /* HAL ticks stayed frozen while RTC advanced ten minutes. Wake only
     * rebases a BATCH request; it must not jump directly to raw streaming. */
    v2_tx_resume(&t,100);
    assert(!t.deferred && t.next_ms==100 && t.phase==V2_TX_BATCH);
    puts("Node wake: sampling/queue/both, independent deadlines, STOP2 HAL clock and fresh permission after wake PASS");
    return 0;
}
