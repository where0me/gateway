#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ws_event_queue.h"
static WsEventQueue g_ws_events;
static char g_ws_msg_buffer[WS_EVENT_BYTES];
static int g_has_new_ws_msg,g_ws_mutex;
static void lock(int *m){assert(!*m);*m=1;}
static void unlock(int *m){assert(*m);*m=0;}
#define pthread_mutex_lock lock
#define pthread_mutex_unlock unlock
#define WEBSOCKET_OP_TEXT 1
struct mg_connection;
struct mg_mgr {struct mg_connection *conns;};
struct mg_connection {struct mg_mgr *mgr;struct mg_connection *next;int is_websocket,is_closing;struct {size_t len;} send;unsigned received;char messages[80][256];};
static void mg_ws_send(struct mg_connection *c,const char *text,size_t length,int kind) {
    assert(g_ws_mutex && kind==1 && length<256 && c->received<80);
    memcpy(c->messages[c->received],text,length);c->messages[c->received++][length]=0;
}
static void queue_ws_event_locked(void);
void task_broadcast_ws(struct mg_connection *);
int main(void) {
    static struct mg_connection a,b,slow,plain;
    struct mg_mgr mgr={.conns=&a};a=(struct mg_connection){.mgr=&mgr,.next=&b,.is_websocket=1};
    b=(struct mg_connection){.mgr=&mgr,.next=&slow,.is_websocket=1};
    slow=(struct mg_connection){.mgr=&mgr,.next=&plain,.is_websocket=1,.send={.len=1024U*1024U+1U}};
    const char *events[]={"{\"type\":\"temp_update\",\"temp\":27}","{\"type\":\"mag_update\"}",
        "{\"type\":\"sound_update\",\"source\":\"digital\"}","{\"type\":\"sound_update\",\"source\":\"analog\"}"};
    for(unsigned i=0;i<4;i++){lock(&g_ws_mutex);strcpy(g_ws_msg_buffer,events[i]);queue_ws_event_locked();unlock(&g_ws_mutex);}
    strcpy(g_ws_msg_buffer,"overwritten scratch");task_broadcast_ws(&a);
    assert(a.received==4 && b.received==4 && !plain.received && !slow.received && slow.is_closing);
    for(unsigned i=0;i<4;i++){assert(!strcmp(a.messages[i],events[i]));assert(!strcmp(b.messages[i],events[i]));}
    assert(!g_has_new_ws_msg && !g_ws_events.count && !g_ws_mutex);
    for(unsigned i=0;i<65;i++){char event[32];snprintf(event,sizeof(event),"{\"event\":%u}",i);assert(ws_event_push(&g_ws_events,event));}
    assert(g_ws_events.count==64 && g_ws_events.overflow==1 && !strcmp(ws_event_front(&g_ws_events),"{\"event\":1}"));
    for(unsigned i=1;i<=64;i++){char event[32];snprintf(event,sizeof(event),"{\"event\":%u}",i);assert(!strcmp(ws_event_front(&g_ws_events),event));ws_event_pop(&g_ws_events);}
    assert(!ws_event_front(&g_ws_events));
    puts("WebSocket actual broadcast PASS: four sensor events in one network tick, FIFO/copy integrity, multiple clients, bounded backlog and slow-client isolation");
}
