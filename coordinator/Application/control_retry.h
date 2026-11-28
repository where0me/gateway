#ifndef CONTROL_RETRY_H
#define CONTROL_RETRY_H
#include <stdint.h>
#include <string.h>
#include "control_sync.h"
typedef struct {
    uint8_t payload[CONTROL_RESUME_CONFIG_SIZE],length,active,inflight,applied,mode;
    uint32_t generation,attempts,next,salt,window_until,created_at;
} ControlRetry;
static inline int control_retry_schedule(ControlRetry *r,const uint8_t *p,unsigned n,uint8_t mode,uint32_t now) {
    if((n!=CONTROL_CONFIG_SIZE && n!=CONTROL_RESUME_CONFIG_SIZE) || mode>CONTROL_DELIVERY_URGENT)return 0;
    /* Editing parameters must not cancel a still-undelivered ON/OFF choice.
     * Replace its payload with the latest settings, retaining urgency. */
    if(r->active && r->mode==CONTROL_DELIVERY_URGENT &&
       !!(r->payload[26]&2U)==!!(p[26]&2U))mode=CONTROL_DELIVERY_URGENT;
    int same=r->length==n && !memcmp(r->payload,p,n);
    if(same && r->active && mode==CONTROL_DELIVERY_URGENT && r->mode==mode)return 1;
    memcpy(r->payload,p,n);r->length=(uint8_t)n;
    if(!++r->generation)++r->generation;
    r->created_at=now;r->mode=mode;r->window_until=now+CONTROL_RECEIVE_WINDOW_MS;
    r->active=mode!=CONTROL_DELIVERY_CACHE;r->applied=0;r->attempts=0;r->next=now;return 1;
}
static inline int control_retry_replace(ControlRetry *r,const uint8_t *p,unsigned n,uint32_t now) {
    return control_retry_schedule(r,p,n,CONTROL_DELIVERY_URGENT,now);
}
/* Application confirmation is stronger than an AF result. Keep inflight
 * until its actual callback so its transaction ID cannot be reused early.
 * Valid periodic reports can also confirm a lost immediate APPLIED report. */
static inline int control_retry_applied(ControlRetry *r,const uint8_t *p,unsigned n) {
    /* 29-byte config: transaction/monitor flags at byte 26, enabled bit 0x02. */
    if((r->length!=CONTROL_CONFIG_SIZE && r->length!=CONTROL_RESUME_CONFIG_SIZE) || r->applied || !control_status_valid(p,n) || !(p[1]&CONTROL_VALID) || (p[1]&CONTROL_NEEDS_SCHEDULE) ||
       control_read_hash(p+2)!=control_fingerprint(r->payload,CONTROL_CONFIG_SIZE) ||
       !!(p[1]&CONTROL_ENABLED)!=!!(r->payload[26]&0x02U))return 0;
    r->applied=1;r->active=0;return 1;
}
static inline int control_retry_due(const ControlRetry *r,uint32_t now) {
    return r->active && !r->inflight && (int32_t)(now-r->next)>=0 &&
        (r->mode==CONTROL_DELIVERY_URGENT ||
         (r->mode==CONTROL_DELIVERY_WINDOW && (int32_t)(now-r->window_until)<0));
}
static inline uint32_t control_retry_delay(uint32_t generation,uint32_t attempts,uint32_t now) {
    uint32_t x=now^(generation*2654435761U)^(attempts*2246822519U);
    x^=x<<13;x^=x>>17;x^=x<<5;return 3000U+x%2001U;
}
static inline void control_retry_result(ControlRetry *r,uint32_t generation,uint8_t status,uint32_t now) {
    r->inflight=0;
    if(generation!=r->generation)return; /* A newer user choice wins. */
    if(r->applied){r->active=0;return;} /* A late failure cannot resurrect retries. */
    if(!status)r->active=0;
    else {
        r->next=now+control_retry_delay(generation^r->salt,r->attempts,now);
        if(r->mode!=CONTROL_DELIVERY_URGENT &&
           (r->mode!=CONTROL_DELIVERY_WINDOW || (int32_t)(r->next-r->window_until)>=0))r->active=0;
    }
}
#endif
