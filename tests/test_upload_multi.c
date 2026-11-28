/* Exercise real UART adapter, per-node receiver and SQLite durability with
   concurrent nodes. Time is virtual; databases are strictly in memory. */
#include <stdio.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <time.h>
#ifdef _WIN32
static struct tm *test_localtime_r(const time_t *v,struct tm *out) {
    struct tm *p=localtime(v);if(!p)return NULL;*out=*p;return out;
}
#define localtime_r test_localtime_r
#endif
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "vibration_tx.h"
#include "vibration_rx.h"
typedef int TestMutex;
static void lock(TestMutex *m){assert(!*m);*m=1;}
static void unlock(TestMutex *m){assert(*m);*m=0;}
#define pthread_mutex_lock lock
#define pthread_mutex_unlock unlock
static TestMutex g_vibration_mutex,g_uart_mutex,g_ws_mutex;
static int g_uart_fd=1,g_has_new_ws_msg;
static char g_ws_msg_buffer[512];
static uint32_t tick;
static int fake_clock(int id,struct timespec *t){(void)id;t->tv_sec=tick/1000;t->tv_nsec=(tick%1000)*1000000;return 0;}
#define clock_gettime fake_clock
#ifndef NODES
#define NODES 4
#endif
#define POINTS 1536
typedef struct {
    V2Tx tx;char mac[24];uint8_t descriptor[V2_BATCH_LEN],raw[POINTS*6],ack[V2_TIMED_ACK_LEN];
    size_t ack_length;
    uint32_t clock_offset,ack_at;
    unsigned completed,grants,waits;
    unsigned epoch;
    uint32_t first_grant;
    int ack_ready,drop_block_ack,drop_window_ack,drop_fragment[V2_MAX_BLOCKS];
} Node;
static Node nodes[NODES];
static unsigned order[1024],order_count;
static unsigned epoch;
static int lookup_mac_by_short(uint16_t addr,char mac[24]) {
    if(addr<0x100 || addr>=0x100+NODES)return 0;
    snprintf(mac,24,"%s",nodes[addr-0x100].mac);return 1;
}
static void publish_collection_data_ready_event(const char *mac,uint32_t batch,int complete) {
    for(unsigned i=0;i<NODES;i++)if(!strcmp(mac,nodes[i].mac)) {
        assert(batch==i+1);if(complete)++nodes[i].completed;return;
    }
    assert(0);
}
static void send_hex_frame_to_uart(int fd,uint8_t cmd,uint8_t len,const uint8_t *data) {
    assert(fd==1 && cmd==V2_ACK && (len==19 || len==39));
    unsigned id=v2_u16(data)-0x100;assert(id<NODES);Node *n=&nodes[id];
    if(data[3]==V2_KIND_BLOCK && data[10]==V2_WAIT)++n->waits;
    if(data[3]==V2_KIND_BLOCK && data[10]==V2_TIMED_OK) {
        n->epoch=epoch;
        if(!n->grants)n->first_grant=tick;
        ++n->grants;if(order_count<1024)order[order_count++]=id;
        if(id==2 && !n->drop_block_ack++)return;
    }
    if(data[3]==V2_KIND_WINDOW && data[10]==V2_OK && id==1 && !n->drop_window_ack++)return;
    n->ack_length=len-2;memcpy(n->ack,data+2,n->ack_length);n->ack_ready=1;
    n->ack_at=tick+20U+(id%4)*15U;
}
#include "vibration_gateway.h"
static int read_raw(void *arg,uint32_t batch,uint32_t first,unsigned count,uint8_t *out) {
    Node *n=arg;assert(batch==v2_u32(n->descriptor+1));assert(first+count<=POINTS);
    memcpy(out,n->raw+first*6,count*6);return 1;
}
static int send_packet(void *arg,uint8_t cmd,const uint8_t *p,size_t count) {
    Node *n=arg;unsigned id=(unsigned)(n-nodes);
    if(cmd==V2_DATA) {
        /* Assert there is never more than one permitted waveform transmitter. */
        unsigned slot=0;for(;slot<V2_GATEWAY_SLOTS;slot++)if(!strcmp(v2_gateway_slots[slot].mac,n->mac))break;
        if(n->epoch==epoch) {
            assert(slot<V2_GATEWAY_SLOTS);
            assert(v2_timed_active(&v2_scheduler,slot,tick));
            /* Data submissions must be in the assigned lane, once per cycle. */
            unsigned phase=(tick-(uint32_t)v2_scheduler.start)%
                (v2_scheduler.count*V2_SLOT_MS+V2_CONTROL_MS);
            int lane=v2_timed_member(&v2_scheduler,slot);
            if(v2_scheduler.count>1) {
                assert(phase>=(unsigned)lane*V2_SLOT_MS+V2_SLOT_OFFSET_MS);
                assert(phase<(unsigned)lane*V2_SLOT_MS+V2_SLOT_OFFSET_MS+V2_SLOT_SUBMIT_MS+V2_SYNC_RTT_MAX_MS/2);
            }
        }
        if(id==1 && p[6]==2 && !n->drop_fragment[v2_u16(p+4)]++)return 1;
    }
    uint8_t uart[81];v2_w16(uart,(uint16_t)(0x100+id));memcpy(uart+2,p,count);
    handle_vibration_v2(cmd,uart,count+2);return 1;
}
static void run(int restart,int disappear) {
    memset(nodes,0,sizeof(nodes));memset(&v2_scheduler,0,sizeof(v2_scheduler));order_count=0;epoch=1;
    assert(init_database()==0);
    for(unsigned i=0;i<NODES;i++) {
        Node *n=&nodes[i];snprintf(n->mac,24,"01:02:03:04:05:06:07:%02X",i);
        assert(insert_new_device(n->mac,"multi-test")==0);
        unsigned r=123+i;for(unsigned k=0;k<sizeof(n->raw);k++){r=r*1664525U+1013904223U;n->raw[k]=(uint8_t)(r>>24);}
        uint8_t *p=n->descriptor;p[0]=V2_VERSION;v2_w32(p+1,i+1);v2_w16(p+5,26667);
        v2_w32(p+7,POINTS);v2_w16(p+11,V2_POINTS);v2_w32(p+13,v2_crc(n->raw,sizeof(n->raw)));
        for(unsigned f=0;f<15;f++)v2_w32(p+17+4*f,UINT32_C(0x80000000));
        n->clock_offset=i*123456U;
        assert(v2_tx_start(&n->tx,p,n->clock_offset));n->tx.rng=i+1;
    }
    int restarted=0,finished=0;
    for(tick=0;tick<3000000;tick+=5) {
        if(restart && !restarted && tick>=85000) {
            for(unsigned i=0;i<NODES;i++)v2_gateway_remove(nodes[i].mac);
            memset(&v2_scheduler,0,sizeof(v2_scheduler));restarted=1;
            ++epoch; /* Nodes keep their state; recovery must use the wire protocol. */
        }
        finished=1;
        for(unsigned i=0;i<NODES;i++) {
            if(disappear && i==0 && nodes[0].grants && tick>=nodes[0].first_grant+100)continue;
            Node *n=&nodes[i];V2TxIO io={n,read_raw,send_packet};
            v2_tx_poll(&n->tx,&io,tick+n->clock_offset);
            if(n->ack_ready && tick>=n->ack_at){n->ack_ready=0;v2_tx_receive(&n->tx,n->ack,n->ack_length,tick+n->clock_offset);}
            assert(n->tx.phase!=V2_TX_ERROR);
            if(n->tx.phase!=V2_TX_DONE)finished=0;
        }
        if(finished)break;
    }
    assert(finished);
    for(unsigned i=0;i<NODES;i++) {
        Node *n=&nodes[i];
        if(!disappear || i!=0){assert(n->completed==1);assert(vibration_batch_matches(n->mac,n->descriptor)==1);}
        v2_gateway_remove(n->mac);
    }
    assert(nodes[1].waits && nodes[2].waits && nodes[3].waits);
    if(!restart && !disappear) {
        unsigned first_group=0;
        for(unsigned i=0;i<NODES;i++)first_group+=nodes[i].first_grant<nodes[0].first_grant+4000U;
        assert(first_group==4); /* Four simultaneous tasks, not one block owner. */
    }
    if(disappear)assert(v2_scheduler.epochs>=2);
    close_database();
    printf("multi-node: nodes=%d restart=%d crashed-owner=%d completed in %u ms PASS\n",NODES,restart,disappear,tick);
}
int main(void){run(0,0);run(1,0);run(0,1);return 0;}
