#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32
static struct tm *localtime_copy(const time_t *time,struct tm *out) {*out=*localtime(time);return out;}
#define localtime_r localtime_copy
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
static uint32_t now_ms;
static int test_clock(int id,struct timespec *out){(void)id;out->tv_sec=now_ms/1000;out->tv_nsec=(now_ms%1000)*1000000;return 0;}
#define clock_gettime test_clock
#define QUEUED_NODES 3
#define TEST_POINTS 1536
typedef struct {
    V2Tx tx; char mac[24]; uint8_t raw[TEST_POINTS*6],batch[V2_BATCH_LEN],ack[V2_ACK_LEN];
    unsigned ack_ready,ack_at,complete,waits,grants,packets;
} TestNode;
static TestNode nodes[QUEUED_NODES];
static int lookup_mac_by_short(uint16_t address,char mac[24]) {
    if(address<0x100 || address>=0x100+QUEUED_NODES)return 0;
    strcpy(mac,nodes[address-0x100].mac);return 1;
}
static void publish_collection_data_ready_event(const char *mac,uint32_t batch,int complete) {
    for(unsigned i=0;i<QUEUED_NODES;i++)if(!strcmp(nodes[i].mac,mac)) {assert(batch==i+1);nodes[i].complete+=complete!=0;return;}
    assert(0);
}
static void send_hex_frame_to_uart(int fd,uint8_t command,uint8_t length,const uint8_t *payload) {
    assert(fd==1 && command==V2_ACK && length==V2_ACK_LEN+2);
    TestNode *node=&nodes[v2_u16(payload)-0x100];
    assert(!node->ack_ready);memcpy(node->ack,payload+2,V2_ACK_LEN);
    node->ack_ready=1;node->ack_at=now_ms+200;
    if(node->ack[1]==V2_KIND_BATCH) {
        node->waits+=node->ack[8]==V2_WAIT;
        node->grants+=node->ack[8]==V2_OK;
    }
}
static unsigned queue_log_count;
static int adapter_printf(const char *format,...) {
    char line[1024];va_list args;va_start(args,format);
    int n=vsnprintf(line,sizeof(line),format,args);va_end(args);
    if(strstr(line,"[上传排队]") || strstr(line,"[排队复查]"))++queue_log_count;
    fputs(line,stdout);return n;
}
#define printf adapter_printf
static uint32_t test_timing_unit(const char *mac,unsigned raw) {
    UploadTimingRecord_t timing;int found=get_upload_timing(mac,raw,&timing);assert(found>=0);
    return found?timing.mean_unit_ms:0U;
}
static void test_timing_save(const char *mac,uint32_t batch,unsigned raw,unsigned blocks,uint64_t elapsed) {
    UploadTimingRecord_t timing;assert(save_upload_timing(mac,batch,raw,blocks,elapsed,&timing)==1);
}
#define UPLOAD_ADMISSION_UNIT_MS(mac,raw) test_timing_unit(mac,raw)
#define UPLOAD_ADMISSION_SAMPLE(e,elapsed) test_timing_save((e)->mac,(e)->batch,(e)->raw_required,(e)->blocks,elapsed)
#include "vibration_gateway.h"
#undef printf
static int read_raw(void *arg,uint32_t batch,uint32_t first,unsigned count,uint8_t *out) {
    TestNode *node=arg;assert(batch==v2_u32(node->batch+1));
    memcpy(out,node->raw+first*6,count*6);return 1;
}
static int send_packet(void *arg,uint8_t command,const uint8_t *payload,size_t length) {
    TestNode *node=arg;
    uint8_t frame[81];v2_w16(frame,(uint16_t)(0x100+(node-nodes)));memcpy(frame+2,payload,length);
    if(command==V2_DATA) {
        assert(upload_owner(&v2_upload_admission,node->mac,v2_u32(node->batch+1),now_ms));
        ++node->packets;
    }
    if(command==V2_BATCH && node==&nodes[1] && node->waits==0) {
        handle_vibration_v2(command,frame,length+2);
        assert(node->ack_ready && node->ack[8]==V2_WAIT);
        node->ack_ready=0; /* lost WAIT: immediate duplicate still receives a reply */
    }
    handle_vibration_v2(command,frame,length+2);return 1;
}
int main(void) {
    assert(init_database()==0);
    for(unsigned i=0;i<QUEUED_NODES;i++) {
        TestNode *node=&nodes[i];snprintf(node->mac,sizeof(node->mac),"01:02:03:04:05:06:07:%02X",i);
        assert(insert_new_device(node->mac,"test")==0);
        for(unsigned j=0;j<sizeof(node->raw);j++)node->raw[j]=(uint8_t)(j*17+i);
        node->batch[0]=V2_VERSION;v2_w32(node->batch+1,i+1);v2_w16(node->batch+5,26667);
        v2_w32(node->batch+7,TEST_POINTS);v2_w16(node->batch+11,V2_POINTS);
        v2_w32(node->batch+13,v2_crc(node->raw,sizeof(node->raw)));
        assert(v2_tx_start(&node->tx,node->batch,0));
    }
    for(now_ms=0;now_ms<120000;now_ms+=10) {
        unsigned completed=0;
        for(unsigned i=0;i<QUEUED_NODES;i++) {
            TestNode *node=&nodes[i];
            if(now_ms<i*40)continue; /* requests are close, not identical */
            if(node->ack_ready && now_ms>=node->ack_at) {
                node->ack_ready=0;v2_tx_receive(&node->tx,node->ack,V2_ACK_LEN,now_ms);
            }
            V2TxIO io={node,read_raw,send_packet};v2_tx_poll(&node->tx,&io,now_ms);
            assert(node->tx.phase!=V2_TX_ERROR);
            completed+=node->tx.phase==V2_TX_DONE;
        }
        if(completed==QUEUED_NODES)break;
    }
    assert(now_ms<120000 && nodes[1].waits==2 && nodes[2].waits==1 && queue_log_count==2);
    for(unsigned i=0;i<QUEUED_NODES;i++) {
        assert(nodes[i].tx.phase==V2_TX_DONE && nodes[i].complete==1 && nodes[i].packets);
        UploadTimingRecord_t timing;
        assert(get_upload_timing(nodes[i].mac,1,&timing)==1 && timing.samples==1 && timing.last_blocks==3);
        v2_gateway_remove(nodes[i].mac);
    }
    close_database();
    puts("Queued gateway: 3 near-simultaneous requests, WAIT/reapply, exclusive bulk sender, durable completion and FIFO PASS");
    return 0;
}
