/* Included once after main.c database/UART/mutex/clock declarations. */
#include "upload_trace_format.h"
#include "vibration_wire.h"
static UploadTraceRing upload_trace_ring;
static UploadTraceLimit upload_trace_limits[64];
static pthread_mutex_t upload_trace_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint32_t upload_trace_snapshot;
static void upload_trace_dump(uint16_t address,uint32_t batch,unsigned reason,int ask_coordinator)
{
    UploadTraceRecord copy[UT_DUMP_MAX];unsigned count=0;
    uint32_t now=(uint32_t)mg_millis(),overwritten;
    pthread_mutex_lock(&upload_trace_mutex);
    if(!ut_allow(upload_trace_limits,64,address,now)){pthread_mutex_unlock(&upload_trace_mutex);return;}
    for(unsigned i=0;i<upload_trace_ring.count && count<UT_DUMP_MAX;i++){
        unsigned pos=(upload_trace_ring.head+UT_RING_SIZE-1U-i)%UT_RING_SIZE;
        UploadTraceRecord *r=&upload_trace_ring.records[pos];
        if(ut_u16(r->p+6)==address)copy[count++]=*r;
    }
    overwritten=upload_trace_ring.overwritten;uint32_t id=++upload_trace_snapshot;
    pthread_mutex_unlock(&upload_trace_mutex);
    char mac[24]={0};(void)lookup_mac_by_short(address,mac);
    printf("[上传链路快照] MAC=%s | 短地址=0x%04X | 批次=%u | 快照=%u | 原因编号=%u | 后端最近记录=%u | 环形覆盖总数=%u\n",
        mac[0]?mac:"未知",address,batch,id,reason,count,overwritten);
    gateway_log_event("WARN","backend","upload_trace_snapshot",mac[0]?mac:NULL,address,batch?batch:-1LL,
        "上传链路快照=%u，原因编号=%u；最近后端记录=%u，详细记录见后端终端日志；有限记录不代表完整空口抓包",id,reason,count);
    while(count){char text[900];UploadTraceRecord *r=&copy[--count];r->p[19]|=UT_SNAPSHOT_FLAG;
        if(ut_format(r->p,UT_LEN,text,sizeof(text)))printf("[上传链路 后端快照=%u] MAC=%s | %s\n",id,mac[0]?mac:"未知",text);
    }
    if(ask_coordinator){uint8_t query[3]={2,(uint8_t)address,(uint8_t)(address>>8)};
        pthread_mutex_lock(&g_uart_mutex);
        if(g_uart_fd>=0)send_hex_frame_to_uart(g_uart_fd,0x15,sizeof(query),query);
        pthread_mutex_unlock(&g_uart_mutex);
    }
}
static void upload_trace_packet(uint8_t stage,uint16_t address,uint8_t cmd,const uint8_t *p,size_t n)
{
    UploadTraceRecord r;
    if(v2_window_query_has_tag(cmd,p,n)) --n;
    if(!ut_packet(&r,stage,(uint32_t)mg_millis(),address,cmd,-1,p,n))return;
    pthread_mutex_lock(&upload_trace_mutex);ut_push(&upload_trace_ring,&r);pthread_mutex_unlock(&upload_trace_mutex);
    if(stage==UT_HOST_RX && cmd==0x8D && n>=6U && p[1]==5U)
        upload_trace_dump(address,ut_u32(r.p+12),2,1);
}
static void upload_trace_expired(const char *mac,uint32_t batch)
{
    uint16_t address;
    /* Called from reservation cleanup; no new database mutation or lease action. */
    if(get_device_short_addr(mac,&address) && address<0xFFF8U)upload_trace_dump(address,batch,3,1);
}
static void upload_trace_reply_for_query(uint16_t address,const uint8_t *reply,int query_seq)
{
    uint8_t wire[2+V2_ACK_LEN+1U];
    size_t length=2U+V2_ACK_LEN;
    ut_w16(wire,address);memcpy(wire+2,reply,V2_ACK_LEN);
    if(query_seq>=0)wire[length++]=(uint8_t)query_seq;
    upload_trace_packet(UT_HOST_REPLY,address,0x0D,reply,V2_ACK_LEN);
    pthread_mutex_lock(&g_uart_mutex);
    int status=send_hex_frame_to_uart_checked(g_uart_fd,0x0D,length,wire);
    pthread_mutex_unlock(&g_uart_mutex);
    UploadTraceRecord r;
    if(ut_packet(&r,UT_HOST_WRITE,(uint32_t)mg_millis(),address,0x0D,-1,reply,17)){
        r.p[11]=status?1U:0U;
        pthread_mutex_lock(&upload_trace_mutex);ut_push(&upload_trace_ring,&r);pthread_mutex_unlock(&upload_trace_mutex);
    }
    if(status)upload_trace_dump(address,ut_u32(reply+2),4,0);
}
static void upload_trace_reply(uint16_t address,const uint8_t *reply)
{
    upload_trace_reply_for_query(address,reply,-1);
}
#define GATEWAY_UPLOAD_TRACE_SEND(address,reply,seq) upload_trace_reply_for_query(address,reply,seq)
