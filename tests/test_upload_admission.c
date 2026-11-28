#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "upload_admission.h"
int main(void) {
    UploadAdmission q={0};
    assert(upload_request(&q,"A",1,27,0)==0);
    uint32_t b=upload_request(&q,"B",2,27,10);
    uint64_t order=q.entries[1].order;
    assert(b>5000 && b<=UPLOAD_QUEUE_MAX_WAIT_MS);
    assert(upload_request(&q,"B",2,27,20)>0 && q.entries[1].order==order);
    assert(upload_request(&q,"C",3,27,30)>b);
    upload_release(&q,"A",1,1000); /* early finish: B is asleep, C cannot jump */
    assert(upload_request(&q,"C",3,27,2000)>0);
    assert(upload_request(&q,"B",2,27,3000)==0);
    assert(upload_owner(&q,"B",2,4000) && !upload_owner(&q,"C",3,4000));
    upload_release(&q,"B",99,5000);assert(upload_owner(&q,"B",2,5000));
    upload_release(&q,"B",2,5000);assert(upload_request(&q,"C",3,27,6000)==0);
    upload_remove(&q,"C");assert(!upload_owner(&q,"C",3,6001));

    memset(&q,0,sizeof(q));assert(!upload_request(&q,"A",1,27,0));
    b=upload_request(&q,"B",2,27,10);
    assert(upload_owner(&q,"A",1,50000)); /* observation does not renew lease */
    assert(!upload_owner(&q,"A",1,60001));
    assert(!upload_request(&q,"B",2,27,60001));
    upload_progress(&q,"B",2,1,110000);
    assert(upload_owner(&q,"B",2,160000));
    assert(!upload_owner(&q,"B",2,170001));

    memset(&q,0,sizeof(q));
    for(unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++) {
        char mac[24];snprintf(mac,sizeof(mac),"node%u",i);
        uint32_t wait=upload_request(&q,mac,i+1,79,0);
        assert((i==0)==(wait==0));assert(wait<=UPLOAD_QUEUE_MAX_WAIT_MS);
    }
    assert(upload_request(&q,"overflow",999,79,0)==30000);
    upload_expire(&q,UPLOAD_QUEUE_MAX_WAIT_MS+30001U);
    assert(!upload_request(&q,"overflow",999,79,UPLOAD_QUEUE_MAX_WAIT_MS+30001U));
    puts("Upload queue: FIFO, relative estimates, duplicate requests, sleeping head, ownership, stale-node expiry and capacity PASS");
    return 0;
}
