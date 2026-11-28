#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "control_retry.h"
#include "upload_lease.h"
#include "link_health.h"
static unsigned events;static const char *last_reason;
#define UPLOAD_ADMISSION_EVENT(e,reason,now) do{(void)(e);(void)(now);events++;last_reason=reason;}while(0)
#include "upload_admission.h"
int main(void) {
    ControlRetry r={0};uint8_t on[29]={1},off[29]={0};uint32_t now=0;
    assert(control_retry_replace(&r,on,29,now));assert(control_retry_due(&r,now));
    r.inflight=1;r.attempts++;uint32_t old=r.generation;
    assert(control_retry_replace(&r,on,29,1) && r.generation==old && r.inflight);
    assert(control_retry_replace(&r,off,29,2) && !control_retry_due(&r,2));
    control_retry_result(&r,old,0,3);assert(r.active && !r.inflight && !r.payload[0]);
    assert(control_retry_due(&r,3));
    for(unsigned i=0;i<1000;i++) {
        r.inflight=1;r.attempts++;control_retry_result(&r,r.generation,0xf0,now);
        assert(r.active && r.next-now>=3000 && r.next-now<=5000);
        assert(!control_retry_due(&r,r.next-1));now=r.next;assert(control_retry_due(&r,now));
    }
    r.inflight=1;control_retry_result(&r,r.generation,0,now);assert(!r.active && !r.inflight);
    assert(!control_retry_replace(&r,on,28,now));
    // Ordinary edits cache the latest payload without generating radio sends.
    ControlRetry deferred={0};on[26]=off[26]=6;
    assert(control_retry_schedule(&deferred,on,29,CONTROL_DELIVERY_CACHE,100));
    assert(!control_retry_due(&deferred,1000000));
    off[0]=3;assert(control_retry_schedule(&deferred,off,29,CONTROL_DELIVERY_CACHE,101));
    assert(deferred.payload[0]==3 && !deferred.active);
    assert(control_retry_schedule(&deferred,off,29,CONTROL_DELIVERY_WINDOW,1000));
    assert(control_retry_due(&deferred,1000) && !control_retry_due(&deferred,9000));
    deferred.inflight=1;deferred.attempts++;
    control_retry_result(&deferred,deferred.generation,0xf0,7500);
    assert(!deferred.active && !control_retry_due(&deferred,1000000));
    // Next wake reopens a bounded window; sleeping never resumes timed retries.
    assert(control_retry_schedule(&deferred,off,29,CONTROL_DELIVERY_WINDOW,200000));
    assert(control_retry_due(&deferred,200000));
    deferred.inflight=1;old=deferred.generation;
    on[0]=4;assert(control_retry_schedule(&deferred,on,29,CONTROL_DELIVERY_CACHE,200001));
    control_retry_result(&deferred,old,0xf0,200002);
    assert(!deferred.active && deferred.payload[0]==4);
    // A pending urgent switch remains urgent when a parameter edit replaces it.
    assert(control_retry_replace(&deferred,on,29,300000));
    off[0]=5;assert(control_retry_schedule(&deferred,off,29,CONTROL_DELIVERY_CACHE,300001));
    assert(deferred.mode==CONTROL_DELIVERY_URGENT && deferred.active && deferred.payload[0]==5);
    assert(!control_retry_schedule(&deferred,off,29,3,300002));
    // Lease ACKs are anchored to request time, not delayed ACK arrival.
    UploadLease l;uint8_t query[10],reply[V2_ACK_LEN];
    upload_lease_start(&l,9,100,0);assert(upload_lease_query(&l,0,query));
    uint32_t first_nonce=l.nonce;
    assert(upload_lease_query(&l,3000,query) && upload_lease_query(&l,6000,query));
    assert(l.nonce==first_nonce && l.sent==0);
    v2_ack(reply,UPLOAD_LEASE_KIND,9,0,V2_OK,(uint64_t)l.nonce|((uint64_t)UPLOAD_LEASE_MS<<32));
    assert(upload_lease_receive(&l,reply,sizeof(reply),10000)==1);
    assert(l.expires==25000 && upload_lease_valid(&l,24999) && !upload_lease_valid(&l,25000));
    assert(upload_lease_query(&l,25000,query));uint32_t nonce=l.nonce;
    assert(upload_lease_receive(&l,reply,sizeof(reply),25001)==0); // old nonce
    v2_ack(reply,UPLOAD_LEASE_KIND,9,0,V2_OK,(uint64_t)nonce|((uint64_t)UPLOAD_LEASE_MS<<32));
    assert(upload_lease_receive(&l,reply,sizeof(reply),50000)==0); // stale grant
    upload_lease_start(&l,9,nonce+1,UINT32_MAX-100U);
    assert(upload_lease_query(&l,UINT32_MAX-100U,query));
    v2_ack(reply,UPLOAD_LEASE_KIND,9,0,V2_OK,(uint64_t)l.nonce|((uint64_t)UPLOAD_LEASE_MS<<32));
    assert(upload_lease_receive(&l,reply,sizeof(reply),100)==1 && upload_lease_valid(&l,100));
    upload_lease_start(&l,9,300,0);assert(!upload_lease_query(&l,20000,query) && l.lost); // dead enqueue path
    UploadAdmission queue={0};assert(!upload_request(&queue,"A",9,27,0));
    assert(upload_request(&queue,"B",10,27,0));
    assert(upload_owner(&queue,"A",9,30000)); // node lease already ended, server holds margin
    upload_progress(&queue,"A",9,1,30000);
    assert(upload_request(&queue,"B",10,27,60001)>0); // renewal extends reservation
    assert(!upload_owner(&queue,"A",9,90001));assert(strstr(last_reason,"60"));
    assert(!upload_request(&queue,"B",10,27,90001));
    LinkHealth h={0};for(unsigned i=0;i<3;i++)link_health_result(&h,0);assert(!h.pending);
    link_health_result(&h,1);assert(!h.failures);
    for(unsigned i=0;i<4;i++)link_health_result(&h,0);assert(h.pending);
    assert(events==3);
    memset(&queue,0,sizeof(queue));assert(!upload_request(&queue,"A",9,27,0));
    upload_revoke(&queue,"A",9,100,"reject");
    assert(upload_request(&queue,"B",10,27,1000)==34100);
    assert(!upload_owner(&queue,"B",10,1000));
    assert(!upload_request(&queue,"B",10,27,35100));
    puts("Reliable control: latest intent, no duplicate inflight, unlimited jittered retries; nonce lease/stale grant/wrap/restart margin; link failure threshold PASS");
}
