#ifndef REPORT_DEDUP_H
#define REPORT_DEDUP_H
#include <stdint.h>
#include <string.h>

/* Bounded presentation/replay cache, never a substitute for protocol ACKs.
 * Single receive-thread use, or protected by the adapter's vibration mutex.
 * Store only after successful processing. Payload bytes, not padded structs,
 * distinguish genuine changes from retransmissions. */
#define REPORT_DEDUP_CAPACITY 256
#define REPORT_DEDUP_BYTES 72
typedef struct {
    char mac[24];
    uint32_t batch;
    uint16_t kind, length;
    uint64_t recorded, order;
    uint8_t payload[REPORT_DEDUP_BYTES];
} ReportDedupEntry;
typedef struct {
    ReportDedupEntry entries[REPORT_DEDUP_CAPACITY];
    uint64_t sequence;
} ReportDedup;
static inline int report_seen(const ReportDedup *cache,const char *mac,uint32_t batch,
                              uint16_t kind,const void *payload,size_t length,
                              uint64_t now,uint64_t window) {
    if(length>REPORT_DEDUP_BYTES)return 0;
    for(unsigned i=0;i<REPORT_DEDUP_CAPACITY;i++) {
        const ReportDedupEntry *e=&cache->entries[i];
        if(e->order && e->batch==batch && e->kind==kind && e->length==length &&
           !strcmp(e->mac,mac) && (!length || !memcmp(e->payload,payload,length)) &&
           (!window || (now>=e->recorded && now-e->recorded<window)))return 1;
    }
    return 0;
}
static inline void report_remember(ReportDedup *cache,const char *mac,uint32_t batch,
                                   uint16_t kind,const void *payload,size_t length,uint64_t now) {
    if(length>REPORT_DEDUP_BYTES)return;
    ReportDedupEntry *target=&cache->entries[0];
    for(unsigned i=0;i<REPORT_DEDUP_CAPACITY;i++) {
        ReportDedupEntry *e=&cache->entries[i];
        if(e->order && e->batch==batch && e->kind==kind && e->length==length &&
           !strcmp(e->mac,mac) && (!length || !memcmp(e->payload,payload,length))) {target=e;break;}
        if(e->order<target->order)target=e;
    }
    memset(target,0,sizeof(*target));
    strncpy(target->mac,mac,sizeof(target->mac)-1);
    target->batch=batch;target->kind=kind;target->length=(uint16_t)length;
    if(length)memcpy(target->payload,payload,length);
    target->recorded=now;target->order=++cache->sequence;
}
static inline const ReportDedupEntry *report_latest_entry(const ReportDedup *cache,
                                                          const char *mac,uint32_t batch,uint16_t kind) {
    const ReportDedupEntry *latest=NULL;
    for(unsigned i=0;i<REPORT_DEDUP_CAPACITY;i++) {
        const ReportDedupEntry *e=&cache->entries[i];
        if(e->order && e->batch==batch && e->kind==kind && !strcmp(e->mac,mac) &&
           (!latest || e->order>latest->order))latest=e;
    }
    return latest;
}
/* Configuration A -> B -> A is a real change, not a replay of old A. */
static inline int report_latest_seen(const ReportDedup *cache,const char *mac,uint32_t batch,
                                     uint16_t kind,const void *payload,size_t length,
                                     uint64_t now,uint64_t window) {
    const ReportDedupEntry *latest=report_latest_entry(cache,mac,batch,kind);
    return latest && length<=REPORT_DEDUP_BYTES && latest->length==length &&
           (!length || !memcmp(latest->payload,payload,length)) &&
           (!window || (now>=latest->recorded && now-latest->recorded<window));
}
static inline void report_forget(ReportDedup *cache,const char *mac) {
    for(unsigned i=0;i<REPORT_DEDUP_CAPACITY;i++)
        if(!strcmp(cache->entries[i].mac,mac))memset(&cache->entries[i],0,sizeof(cache->entries[i]));
}
#endif
