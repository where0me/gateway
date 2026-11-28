#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "upload_trace.h"
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
static void pthread_mutex_lock(int *p){assert(!*p);*p=1;}
static void pthread_mutex_unlock(int *p){assert(*p);*p=0;}
static uint32_t clock_ms;static uint64_t mg_millis(void){return clock_ms;}
static int g_uart_fd=3,g_uart_mutex,write_status,write_count,queries,summaries;
static uint8_t last_reply[20],last_length;
static int lookup_mac_by_short(uint16_t address,char mac[24]){
    if(address!=0x714C)return 0;strcpy(mac,"97:56:EF:00:25:E1:80:00");return 1;
}
static int get_device_short_addr(const char *mac,uint16_t *addr){
    if(strcmp(mac,"97:56:EF:00:25:E1:80:00"))return 0;*addr=0x714C;return 1;
}
static int send_hex_frame_to_uart_checked(int fd,unsigned char cmd,unsigned char n,const unsigned char *p){
    assert(g_uart_mutex && fd==g_uart_fd && cmd==0x0D && (n==19 || n==20));
    write_count++;last_length=n;memcpy(last_reply,p,n);
    return fd<0?-1:write_status;
}
static void send_hex_frame_to_uart(int fd,unsigned char cmd,unsigned char n,const unsigned char *p){
    assert(g_uart_mutex && fd==3 && cmd==0x15 && n==3 && p[0]==2 && ut_u16(p+1)==0x714C);queries++;
}
static void gateway_log_event(const char *level,const char *source,const char *event,const char *mac,int address,long long batch,const char *fmt,...){
    (void)batch;(void)fmt;assert(!strcmp(level,"WARN") && !strcmp(source,"backend"));
    assert(!strcmp(event,"upload_trace_snapshot") && mac && address==0x714C);summaries++;
}
static char console[80000];static size_t console_length;
static int trace_printf(const char *fmt,...){
    va_list ap;va_start(ap,fmt);int n=vsnprintf(console+console_length,sizeof(console)-console_length,fmt,ap);va_end(ap);
    assert(n>=0 && (size_t)n<sizeof(console)-console_length);console_length+=(size_t)n;return n;
}
#define printf trace_printf
#include "upload_trace_backend.h"
#undef printf
int main(void)
{
    uint8_t data[79]={0};ut_w32(data,42);ut_w16(data+4,11);data[6]=19;
    for(unsigned i=0;i<130;i++){clock_ms=i;upload_trace_packet(UT_HOST_RX,0x714C,0x94,data,79);}
    assert(!console_length && !summaries && !queries && upload_trace_ring.count==96);
    uint8_t reply[17]={2,3};ut_w32(reply+2,42);ut_w16(reply+6,11);
    upload_trace_reply(0x714C,reply);assert(write_count==1 && !console_length && !summaries);
    assert(last_length==19 && ut_u16(last_reply)==0x714C && !memcmp(last_reply+2,reply,17));
    upload_trace_reply_for_query(0x714C,reply,35);
    assert(last_length==20 && last_reply[19]==35 && !memcmp(last_reply+2,reply,17));
    uint8_t query[9]={2,3,42,0,0,0,11,0,35};
    upload_trace_packet(UT_HOST_RX,0x714C,0x95,query,sizeof(query));
    unsigned pos=(upload_trace_ring.head+UT_RING_SIZE-1)%UT_RING_SIZE;
    assert(ut_u32(upload_trace_ring.records[pos].p+20)==ut_fingerprint(query,8));
    pos=(pos+UT_RING_SIZE-1)%UT_RING_SIZE;
    assert(upload_trace_ring.records[pos].p[1]==UT_HOST_WRITE && !upload_trace_ring.records[pos].p[11]);
    clock_ms=200;upload_trace_expired("97:56:EF:00:25:E1:80:00",42);
    assert(summaries==1 && queries==1 && strstr(console,"分片=20") && strstr(console,"后端生成回复") && strstr(console,"串口=完整写入"));
    size_t before=console_length;upload_trace_expired("97:56:EF:00:25:E1:80:00",42);
    assert(summaries==1 && queries==1 && console_length==before);
    clock_ms+=10000;write_status=-1;upload_trace_reply(0x714C,reply);
    assert(summaries==2 && queries==1 && strstr(console,"串口=未完整写入"));
    clock_ms+=10000;uint8_t error[6]={1,5,42};
    upload_trace_packet(UT_HOST_RX,0x714C,0x8D,error,6);
    assert(summaries==3 && queries==2);
    assert(!upload_trace_mutex && !g_uart_mutex);
    puts("Backend trace PASS: quiet normal flow, exact reply bytes, checked local UART result, timeout/error snapshots, cooldown and lock ownership");
}
