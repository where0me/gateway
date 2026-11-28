#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32
/* Test harness runs single-threaded on Windows; production uses localtime_r. */
static struct tm *test_localtime_r(const time_t *value, struct tm *out) {
    struct tm *result=localtime(value);
    if(!result)return NULL;
    *out=*result;return out;
}
#define localtime_r test_localtime_r
#endif
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "vibration_tx.h"
#include "vibration_rx.h"
/* Single-threaded adapter harness. Production mutexes remain real pthread mutexes. */
typedef int TestMutex;
static void test_mutex_lock(TestMutex *m){assert(*m==0);*m=1;}
static void test_mutex_unlock(TestMutex *m){assert(*m==1);*m=0;}
#define pthread_mutex_t TestMutex
#define pthread_mutex_lock test_mutex_lock
#define pthread_mutex_unlock test_mutex_unlock
static pthread_mutex_t g_vibration_mutex,g_uart_mutex,g_ws_mutex;
static int g_uart_fd=1,g_has_new_ws_msg;
static char g_ws_msg_buffer[512];
static uint8_t ack[V2_ACK_LEN];
static size_t ack_length;
static int ack_ready;
static uint32_t test_now,ack_at;
static unsigned dropped_fragment,missing_replies,data_packets;
static unsigned completed;
static unsigned fault_mode, dropped_ack, dropped_descriptor, queries;
static int expected_query_seq=-1;
static unsigned tagged_replies;
static int lookup_mac_by_short(uint16_t address,char mac[24]) {
    char short_text[16];snprintf(short_text,sizeof(short_text),"%04X",address);
    return get_mac_by_short_addr(short_text,mac);
}
static void publish_collection_data_ready_event(const char *mac,uint32_t batch,int complete) {
    (void)mac;assert(batch==9);if(complete)completed++;
}
static void send_hex_frame_to_uart(int fd,uint8_t cmd,uint8_t len,const uint8_t *data) {
    assert(fd==1 && cmd==V2_ACK && v2_u16(data)==0x4321);
    if(expected_query_seq>=0) {
        assert(len==20 && data[3]==V2_KIND_WINDOW && data[19]==expected_query_seq);
        ++tagged_replies;
    } else assert(len==19);
    assert(!ack_ready);
    if(fault_mode==3 && !dropped_ack && data[3]==V2_KIND_WINDOW &&
       data[10]==V2_OK && v2_u16(data+8)==0) {
        dropped_ack=1;return; /* A committed block's OK was lost. */
    }
    if(data[10]==V2_MISSING)++missing_replies;
    ack_at=test_now+400U; /* Ordinary sleepy-parent delay must not reject grants. */
    ack_length=V2_ACK_LEN;memcpy(ack,data+2,ack_length);ack_ready=1;
}
static int test_clock(int id,struct timespec *t) {
    (void)id;t->tv_sec=test_now/1000;t->tv_nsec=(test_now%1000)*1000000;return 0;
}
#define clock_gettime test_clock
static unsigned reception_logged,fragments_logged,verified_logged;
static int adapter_printf(const char *format,...) {
    char line[1024];va_list args;va_start(args,format);
    int n=vsnprintf(line,sizeof(line),format,args);va_end(args);
    if(strstr(line,"首个波形分片已到达")) {assert(!reception_logged);reception_logged++;}
    if(strstr(line,"包全部收到")) {assert(reception_logged && !fragments_logged && !verified_logged);fragments_logged++;}
    if(strstr(line,"已校验 1/")) {assert(fragments_logged);verified_logged++;}
    fputs(line,stdout);return n;
}
#define printf adapter_printf
#include "vibration_gateway.h"
#undef printf
static uint8_t raw[1025*6],descriptor[V2_BATCH_LEN];
static int read_block(void *arg,uint32_t batch,uint32_t first,unsigned points,uint8_t *out) {
    (void)arg;assert(batch==9 && first+points<=1025);memcpy(out,raw+first*6,points*6);return 1;
}
static int send_packet(void *arg,uint8_t command,const uint8_t *data,size_t n) {
    (void)arg;uint8_t uart[81];v2_w16(uart,0x4321);memcpy(uart+2,data,n);
    int had_ack=ack_ready;
    if(command==V2_BLOCK) {
        assert(n==V2_BLOCK_LEN);
        if(fault_mode==4 && !dropped_descriptor && v2_u16(data+5)==1) {
            dropped_descriptor=1;return 1;
        }
    }
    if(command==V2_QUERY && data[1]==V2_KIND_WINDOW)++queries;
    if(command==V2_DATA) {
        ++data_packets;
        unsigned fragments=0;
        for(unsigned i=0;i<V2_GATEWAY_SLOTS;++i)
            if(v2_gateway_slots[i].rx.have_block &&
               v2_gateway_slots[i].rx.block_index==v2_u16(data+4))
                fragments=v2_gateway_slots[i].rx.fragments;
        /* A persisted block may send BLOCK_DONE while the node starts data. */
        assert(!ack_ready || (ack[1]==V2_KIND_BLOCK && ack[8]==V2_BLOCK_DONE));
        if(!dropped_fragment && v2_u16(data+4)==0 &&
           ((fault_mode==1 && data[6]==2) ||
            (fault_mode==2 && data[6]+1U==fragments) ||
            (fault_mode==5 && data[6]==0))) {
            dropped_fragment=1;return 1;
        }
        if(fault_mode==6 && !dropped_fragment && v2_u16(data+4)==0 && data[6]==0) {
            uart[9]^=1;dropped_fragment=1; /* complete frame set, invalid block CRC */
        }
    }
    if(fault_mode==3 && command==V2_QUERY && n==8 && data[1]==V2_KIND_WINDOW) {
        expected_query_seq=35;
        uart[2U+n++]=(uint8_t)expected_query_seq;
    }
    handle_vibration_v2(command,uart,n+2);
    expected_query_seq=-1;
    if(command==V2_DATA && ack_ready && !had_ack) {
        /* Only completion/CRC failure of an entire block generates a reply. */
        assert(ack[1]==V2_KIND_WINDOW);
        int done=0;
        for(unsigned i=0;i<V2_GATEWAY_SLOTS;++i)
            done|=v2_gateway_slots[i].rx.blocks_done[v2_u16(data+4)];
        assert(done || ack[8]==V2_MISSING);
    }
    return 1;
}
int main(void) {
    const char *mac="01:02:03:04:05:06:07:08";
    assert(init_database()==0);assert(insert_new_device(mac,"test")==0);
    assert(update_device_online(mac,"4321")==0);
    for(unsigned i=0;i<sizeof(raw);i++)raw[i]=(uint8_t)(i*17);
    descriptor[0]=V2_VERSION;v2_w32(descriptor+1,9);v2_w16(descriptor+5,26667);
    v2_w32(descriptor+7,1025);v2_w16(descriptor+11,512);
    v2_w32(descriptor+13,v2_crc(raw,sizeof(raw)));
    for(unsigned i=0;i<15;i++)v2_w32(descriptor+17+i*4,UINT32_C(0x80000000));
    V2Tx tx;V2TxIO io={NULL,read_block,send_packet};
    for(unsigned run=0;run<14;run++) {
        fault_mode=run/2;
        if(run%2==0) {
            if(run) {
                assert(delete_device(mac)==0);
                assert(insert_new_device(mac,"test")==0);
                assert(update_device_online(mac,"4321")==0);
            }
            completed=dropped_fragment=missing_replies=dropped_ack=dropped_descriptor=queries=0;
            reception_logged=fragments_logged=verified_logged=0;
        }
        assert(v2_tx_start(&tx,descriptor,0));
        for(uint32_t now=0;now<300000 && v2_tx_active(&tx);now+=5) {
            test_now=now;
            v2_tx_poll(&tx,&io,now);
            v2_gateway_tick();
            if(ack_ready && v2_due(now,ack_at)){v2_tx_receive(&tx,ack,ack_length,now);ack_ready=0;}
        }
        assert(tx.phase==V2_TX_DONE);
        assert(vibration_batch_matches(mac,descriptor)==1);
        assert(completed==1);
        if(run%2==0) {
            assert(reception_logged==1 && fragments_logged==1 && verified_logged==1);
            if(fault_mode==0)assert(queries==0 && missing_replies==0);
            if(fault_mode==1 || fault_mode==2 || fault_mode==5 || fault_mode==6)
                assert(dropped_fragment==1 && missing_replies==1 && queries==0);
            if(fault_mode==3)assert(dropped_ack==1 && queries==1 && tagged_replies==1);
            if(fault_mode==4)assert(dropped_descriptor==1 && queries==1);
        }
        int32_t final_features[3][5]={{1234}};
        assert(save_vibration_batch(mac,9,26667,1025,NULL,0,final_features)==0);
        assert(vibration_batch_matches(mac,descriptor)==1);
        v2_gateway_remove(mac); /* Gateway restart: persisted batch re-ACKs without rewriting. */
        /* The next run resets its monotonic clock to zero. A real restart also
         * resets admission memory; do not carry a previous clock's fence. */
        memset(&v2_upload_admission,0,sizeof(v2_upload_admission));
    }
    assert(data_packets>0);
    assert(delete_device(mac)==0);
    v2_gateway_remove(mac);ack_ready=0;
    send_packet(NULL,V2_BATCH,descriptor,V2_BATCH_LEN);
    assert(!ack_ready && vibration_batch_matches(mac,descriptor)==0);
    close_database();
    puts("gateway adapter: 400 ms replies, zero healthy block queries, lost middle/tail/OK/metadata recovery, pushed block ACK, SQL store, restart, removal PASS");
    return 0;
}
