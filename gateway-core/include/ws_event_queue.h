#ifndef WS_EVENT_QUEUE_H
#define WS_EVENT_QUEUE_H
#include <stddef.h>
#include <string.h>
#define WS_EVENT_CAPACITY 64U
#define WS_EVENT_BYTES 8192U
/* Caller holds the existing WebSocket mutex. Complete JSON events are copied,
 * never pointers into producers' shared scratch buffers. */
typedef struct {char events[WS_EVENT_CAPACITY][WS_EVENT_BYTES];unsigned head,count,overflow;} WsEventQueue;
static inline int ws_event_push(WsEventQueue *q,const char *event) {
    size_t length=strlen(event);if(length>=WS_EVENT_BYTES){q->overflow++;return 0;}
    if(q->count==WS_EVENT_CAPACITY){q->head=(q->head+1U)%WS_EVENT_CAPACITY;q->count--;q->overflow++;}
    unsigned tail=(q->head+q->count)%WS_EVENT_CAPACITY;
    memcpy(q->events[tail],event,length+1U);q->count++;return 1;
}
static inline const char *ws_event_front(const WsEventQueue *q){return q->count?q->events[q->head]:NULL;}
static inline void ws_event_pop(WsEventQueue *q){if(q->count){q->head=(q->head+1U)%WS_EVENT_CAPACITY;q->count--;}}
#endif
