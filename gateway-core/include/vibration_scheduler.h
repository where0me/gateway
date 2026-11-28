#ifndef VIBRATION_SCHEDULER_H
#define VIBRATION_SCHEDULER_H
#include "vibration_wire.h"
#define V2_SCHEDULER_SLOTS 16U
typedef struct {
    uint64_t ticket, seen;
} V2UploadWaiter;
typedef struct {
    V2UploadWaiter waiters[V2_SCHEDULER_SLOTS];
    uint64_t sequence, until, not_before;
    int initialized;
    unsigned owner, block; /* owner = gateway slot index + 1, zero means free */
    uint32_t grants, waits, expirations;
} V2UploadScheduler;
static inline void v2_schedule_expire(V2UploadScheduler *s, uint64_t now) {
    /* After a backend restart an old node may still hold a grant. Drain its
       maximum lifetime before issuing any new one; WAIT remains responsive. */
    if(!s->initialized){s->initialized=1;s->not_before=now+V2_GRANT_MS+V2_GRANT_GUARD_MS;}
    if(s->owner && now>=s->until){s->owner=0;++s->expirations;}
    for(unsigned i=0;i<V2_SCHEDULER_SLOTS;i++)
        if(s->waiters[i].ticket && now-s->waiters[i].seen>10000U)
            s->waiters[i].ticket=0; /* dead/sleeping requester cannot block others */
}
static inline int v2_schedule_owns(V2UploadScheduler *s,unsigned id,unsigned block,uint64_t now) {
    v2_schedule_expire(s,now);
    return s->owner==id+1U && s->block==block;
}
static inline void v2_schedule_forget(V2UploadScheduler *s,unsigned id) {
    s->waiters[id].ticket=0;
    if(s->owner==id+1U)s->owner=0;
}
static inline int v2_schedule_request(V2UploadScheduler *s,unsigned id,unsigned block,uint64_t now) {
    v2_schedule_expire(s,now);
    if(s->owner==id+1U && s->block==block){
        s->until=now+V2_GRANT_MS+V2_GRANT_GUARD_MS;return 1;
    }
    V2UploadWaiter *w=&s->waiters[id];
    if(!w->ticket)w->ticket=++s->sequence;
    w->seen=now;
    if(!s->owner && now>=s->not_before){
        unsigned first=id;
        for(unsigned i=0;i<V2_SCHEDULER_SLOTS;i++)
            if(s->waiters[i].ticket && s->waiters[i].ticket<s->waiters[first].ticket)first=i;
        if(first==id){
            w->ticket=0;s->owner=id+1U;s->block=block;
            s->until=now+V2_GRANT_MS+V2_GRANT_GUARD_MS;++s->grants;return 1;
        }
    }
    ++s->waits;return 0;
}
static inline void v2_schedule_release(V2UploadScheduler *s,unsigned id,unsigned block) {
    if(s->owner==id+1U && s->block==block)s->owner=0;
}
#endif
