#include <assert.h>
#include <stdio.h>
#include "scalar_tx.h"
#include "upload_lease.h"
static unsigned manifests;
static int blocked;
static int item(void *c,unsigned i){(void)c;(void)i;return 1;}
static int manifest(void *c,const uint8_t *p){(void)c;(void)p;if(blocked)return 0;manifests++;return 1;}
int main(void) {
    uint8_t p[SCALAR_MANIFEST_LEN]={2},query[10],reply[V2_ACK_LEN];
    scalar_w32(p+1,783436875U);p[5]=1;p[6]=p[7]=SCALAR_ALL;
    ScalarTx scalar;ScalarTxIO io={NULL,item,manifest};UploadLease lease;
    assert(scalar_tx_start(&scalar,p,1000));
    upload_lease_start(&lease,783436875U,100,1000);
    assert(upload_lease_query(&lease,1000,query));
    v2_ack(reply,UPLOAD_LEASE_KIND,lease.batch,0,V2_OK,(uint64_t)lease.nonce|((uint64_t)UPLOAD_LEASE_MS<<32));
    assert(upload_lease_receive(&lease,reply,sizeof(reply),19047)==1);
    assert(lease.expires-19047==6953); // Replay 0E's real delayed confirmation.
    assert(!scalar_tx_poll(&scalar,&io,19048) && manifests==1 && scalar.attempts==1 && !scalar.error);
    // Queue wait can be minutes and wrap the tick counter before first use.
    assert(scalar_tx_start(&scalar,p,UINT32_MAX-2000U));
    assert(!scalar_tx_poll(&scalar,&io,180000) && !scalar.error);
    // Once the stage starts, blocked enqueue still has a finite 10s budget.
    assert(scalar_tx_start(&scalar,p,0));blocked=1;
    assert(!scalar_tx_poll(&scalar,&io,20000));uint32_t deadline=scalar.deadline_ms;
    assert(!scalar_tx_poll(&scalar,&io,29999) && scalar.deadline_ms==deadline);
    assert(scalar_tx_poll(&scalar,&io,30000)==-1);
    // A new transaction resets once; lease renewals do not restart the stage.
    blocked=0;assert(scalar_tx_start(&scalar,p,30001));
    assert(!scalar_tx_poll(&scalar,&io,40000) && scalar.attempts==1);
    puts("Scalar/lease timing PASS: delayed 18.047s grant, long sleep/wrap, one-time stage start and bounded backpressure");
}
