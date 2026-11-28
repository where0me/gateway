#ifndef UPLOAD_ADMISSION_H
#define UPLOAD_ADMISSION_H
#include <stdint.h>
#include <string.h>
#define UPLOAD_QUEUE_CAPACITY 64
#define UPLOAD_QUEUE_IDLE_MS 60000U
#define UPLOAD_QUEUE_MAX_WAIT_MS 1800000U
#ifndef UPLOAD_ADMISSION_EVENT
#define UPLOAD_ADMISSION_EVENT(entry,reason,now) ((void)(entry),(void)(reason),(void)(now))
#endif
#ifndef UPLOAD_ADMISSION_UNIT_MS
#define UPLOAD_ADMISSION_UNIT_MS(mac,raw) (0U)
#endif
#ifndef UPLOAD_ADMISSION_SAMPLE
#define UPLOAD_ADMISSION_SAMPLE(entry,elapsed) ((void)(entry),(void)(elapsed))
#endif
typedef struct UploadAdmissionEntry {
    char mac[24];
    uint32_t batch, blocks, done;
    uint64_t order, expires, started, touched, expected_end, scalar_verified_ms;
    unsigned active,raw_required,raw_seen;
} UploadAdmissionEntry;
typedef struct {
    UploadAdmissionEntry entries[UPLOAD_QUEUE_CAPACITY];
    uint64_t sequence,handover_until;
} UploadAdmission;
static inline uint64_t upload_duration(const UploadAdmissionEntry *entry) {
    uint32_t unit=UPLOAD_ADMISSION_UNIT_MS(entry->mac,entry->raw_required);
    if(!unit)unit=entry->raw_required?3000U:5000U;
    return (uint64_t)unit*(entry->raw_required?entry->blocks:1U)+5000U;
}
static inline void upload_expire(UploadAdmission *q, uint64_t now) {
    for (unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++) {
        UploadAdmissionEntry *e=&q->entries[i];
        if (e->mac[0] && (e->active ? now-e->touched>UPLOAD_QUEUE_IDLE_MS : now>e->expires)) {
            UPLOAD_ADMISSION_EVENT(e,e->active?"60秒无上传活动，许可过期":"排队申请过期",now);
            memset(e,0,sizeof(*e));
        }
    }
}
static inline UploadAdmissionEntry *upload_owner_entry(UploadAdmission *q,const char *mac,uint32_t batch,uint64_t now) {
    upload_expire(q,now);
    for (unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++) {
        UploadAdmissionEntry *e=&q->entries[i];
        if(e->active && e->batch==batch && !strcmp(e->mac,mac)) return e;
    }
    return NULL;
}
static inline int upload_owner(UploadAdmission *q,const char *mac,uint32_t batch,uint64_t now) {
    return upload_owner_entry(q,mac,batch,now)!=NULL;
}
/* Zero = permission now. Positive = relative retry estimate, never permission. */
static inline uint32_t upload_request_kind(UploadAdmission *q,const char *mac,uint32_t batch,uint32_t blocks,unsigned raw,uint64_t now) {
    upload_expire(q,now);
    UploadAdmissionEntry *entry=NULL,*free_entry=NULL,*first=NULL,*owner=NULL;
    for(unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++) {
        UploadAdmissionEntry *e=&q->entries[i];
        if(!e->mac[0]) {if(!free_entry)free_entry=e;continue;}
        if(e->active)owner=e;
        if(!strcmp(e->mac,mac) && e->batch==batch)entry=e;
        if(!first || e->order<first->order)first=e;
    }
    if(!entry) {
        if(!free_entry)return 30000U;
        entry=free_entry;strncpy(entry->mac,mac,sizeof(entry->mac)-1);
        entry->batch=batch;entry->blocks=blocks ? blocks : 1;
        entry->raw_required=raw!=0U;
        entry->order=++q->sequence;
        if(!first)first=entry;
    }
    if(!owner && now<q->handover_until) {
        entry->expires=q->handover_until+30000U;
        return (uint32_t)(q->handover_until-now);
    }
    if(entry==owner || (!owner && first==entry)) {
        if(!entry->active) {
            entry->active=1;entry->started=now;
            entry->expected_end=now+upload_duration(entry);
            entry->touched=now;
            UPLOAD_ADMISSION_EVENT(entry,"取得上传名额",now);
        }
        entry->touched=now;return 0;
    }
    uint64_t wait=1000U;
    for(unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++) {
        UploadAdmissionEntry *e=&q->entries[i];
        if(!e->mac[0] || e==entry)continue;
        if(e->active)wait+=e->expected_end>now ? e->expected_end-now : 5000U;
        else if(e->order<entry->order) {
            /* An early finish must not let newcomers bypass a sleeping head. */
            if(!owner && e==first && e->expires>now+30000U)wait+=e->expires-now-30000U;
            wait+=upload_duration(e);
        }
    }
    if(wait>UPLOAD_QUEUE_MAX_WAIT_MS)wait=UPLOAD_QUEUE_MAX_WAIT_MS;
    entry->expires=now+wait+30000U;
    return (uint32_t)wait;
}
/* Legacy descriptors/tests have no raw-policy capability; use the conservative
 * waveform estimate. Enhanced descriptors explicitly distinguish feature-only. */
static inline uint32_t upload_request(UploadAdmission *q,const char *mac,uint32_t batch,uint32_t blocks,uint64_t now) {
    return upload_request_kind(q,mac,batch,blocks,1U,now);
}
static inline void upload_progress(UploadAdmission *q,const char *mac,uint32_t batch,unsigned done,uint64_t now) {
    for(unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++) {
        UploadAdmissionEntry *e=&q->entries[i];
        if(!e->active || e->batch!=batch || strcmp(e->mac,mac))continue;
        e->touched=now;
        if(done) {
            if(done>e->done)e->done=done;
            uint64_t duration=(now-e->started)*e->blocks/done+5000U;
            e->expected_end=e->started+duration;
        }
    }
}
static inline void upload_release_reason(UploadAdmission *q,const char *mac,uint32_t batch,uint64_t now,const char *reason) {
    for(unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++) {
        UploadAdmissionEntry *e=&q->entries[i];
        if(e->batch!=batch || strcmp(e->mac,mac))continue;
        UPLOAD_ADMISSION_EVENT(e,reason,now);
        memset(e,0,sizeof(*e));
    }
}
/* Only a verified successful finish teaches timing. Revocation, inactivity,
 * removal and durable-completion replay merely release the reservation. */
static inline void upload_finish(UploadAdmission *q,const char *mac,uint32_t batch,uint64_t now,const char *reason) {
    for(unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++) {
        UploadAdmissionEntry *e=&q->entries[i];
        if(e->active && e->batch==batch && !strcmp(e->mac,mac) && now>e->started &&
           (!e->raw_required || (e->raw_seen && e->done>=e->blocks)))
            UPLOAD_ADMISSION_SAMPLE(e,now-e->started);
    }
    upload_release_reason(q,mac,batch,now,reason);
}
static inline void upload_raw_seen(UploadAdmission *q,const char *mac,uint32_t batch) {
    for(unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++) {
        UploadAdmissionEntry *e=&q->entries[i];
        if(e->active && e->batch==batch && !strcmp(e->mac,mac))e->raw_seen=1U;
    }
}
static inline void upload_release(UploadAdmission *q,const char *mac,uint32_t batch,uint64_t now) {
    upload_release_reason(q,mac,batch,now,"上传结束，释放名额");
}
/* A REJECT ACK or removal notification may be lost. Let any previous node
 * lease (25s from its request) expire before allowing another bulk sender. */
static inline void upload_revoke(UploadAdmission *q,const char *mac,uint32_t batch,uint64_t now,const char *reason) {
    if(upload_owner(q,mac,batch,now))q->handover_until=now+35000U;
    upload_release_reason(q,mac,batch,now,reason);
}
static inline void upload_remove(UploadAdmission *q,const char *mac) {
    for(unsigned i=0;i<UPLOAD_QUEUE_CAPACITY;i++)
        if(!strcmp(q->entries[i].mac,mac)) {
            if(q->entries[i].active && q->handover_until<q->entries[i].touched+35000U)
                q->handover_until=q->entries[i].touched+35000U;
            UPLOAD_ADMISSION_EVENT(&q->entries[i],"节点移除，取消上传名额或排队",q->entries[i].touched);
            memset(&q->entries[i],0,sizeof(q->entries[i]));
        }
}
#endif
