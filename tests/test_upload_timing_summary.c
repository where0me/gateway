#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
static struct tm *copy_localtime(const time_t *t,struct tm *out){*out=*localtime(t);return out;}
#define localtime_r copy_localtime
#endif
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "vibration_rx.h"
#include "mqtt_store.h"
static const char *mac="01:02:03:04:05:06:07:08";
static uint32_t now_ms=40000;
static int test_clock(int id,struct timespec *out){(void)id;out->tv_sec=now_ms/1000;out->tv_nsec=(now_ms%1000)*1000000;return 0;}
#define clock_gettime test_clock
typedef int TestMutex;
static void lock(TestMutex *m){assert(!*m);*m=1;}
static void unlock(TestMutex *m){assert(*m);*m=0;}
#define pthread_mutex_lock lock
#define pthread_mutex_unlock unlock
static TestMutex g_vibration_mutex,g_uart_mutex,g_ws_mutex;
static int g_uart_fd=1,g_has_new_ws_msg;
static char g_ws_msg_buffer[1024];
static uint8_t ack[V2_ACK_LEN];
static int lookup_mac_by_short(uint16_t addr,char out[24]){if(addr!=0x1234)return 0;strcpy(out,mac);return 1;}
static void publish_collection_data_ready_event(const char *m,uint32_t batch,int raw){(void)m;(void)batch;(void)raw;}
static void send_hex_frame_to_uart(int fd,uint8_t cmd,uint8_t len,const uint8_t *p)
{assert(fd==1 && cmd==V2_ACK && len==V2_ACK_LEN+2);memcpy(ack,p+2,V2_ACK_LEN);}
static uint32_t timing_unit(const char *m,unsigned raw){UploadTimingRecord_t t;int found=get_upload_timing(m,raw,&t);assert(found>=0);return found?t.mean_unit_ms:0;}
static char timing_message[1024];
void gateway_log_event(const char *level,const char *source,const char *event,
                       const char *m,int address,int64_t batch,const char *format,...)
{
    (void)level;(void)source;(void)m;(void)address;(void)batch;
    assert(!strcmp(event,"upload_timing_saved"));
    va_list args;
    va_start(args,format);
    vsnprintf(timing_message,sizeof(timing_message),format,args);
    va_end(args);
}
struct UploadAdmissionEntry;
static void upload_timing_sample(const struct UploadAdmissionEntry *entry,uint64_t elapsed);
#define UPLOAD_ADMISSION_UNIT_MS(mac,raw) timing_unit(mac,raw)
#define UPLOAD_ADMISSION_SAMPLE(e,elapsed) upload_timing_sample(e,elapsed)
#include "vibration_gateway.h"

/* Legacy assembly is outside this test; V2 uses the real receiver above. */
typedef struct {uint16_t sample_rate_hz,received_packets,packet_count;uint32_t sample_count;int32_t features[3][5];int summary_received;time_t last_update;} VibrationAssembly_t;
static VibrationAssembly_t *find_vibration_assembly(uint16_t a,uint32_t b,uint16_t n){(void)a;(void)b;(void)n;return NULL;}
static void finish_vibration_assembly(VibrationAssembly_t *s){(void)s;assert(0);}
static void mqtt_note_batch(const char *m,uint32_t batch,enum MqttBatchEvent event){(void)m;(void)batch;(void)event;}
static void upload_trace_reply(uint16_t address,const uint8_t *reply){assert(address==0x1234);memcpy(ack,reply,V2_ACK_LEN);}
#define read_le16 v2_u16
#define read_le32 v2_u32
#define VIBRATION_MAX_SAMPLE_COUNT 40001U
#define VIBRATION_SAMPLES_PER_PACKET 10U
void handle_cmd_8c_vibration_summary(unsigned char *,unsigned char);
static uint8_t raw[12]={1,0,2,0,3,0,4,0,5,0,6,0};
static void batch(uint32_t id,unsigned waveform) {
    uint8_t p[2+UPLOAD_COMPLETION_BATCH_LEN]={0};v2_w16(p,0x1234);
    uint8_t *b=p+2;b[0]=V2_VERSION;v2_w32(b+1,id);v2_w16(b+5,26667);v2_w32(b+7,2);
    v2_w16(b+11,V2_POINTS);v2_w32(b+13,v2_crc(raw,sizeof(raw)));
    for(unsigned i=0;i<15;i++)v2_w32(b+17+4*i,0x80000000U);
    b[V2_BATCH_LEN]=UPLOAD_COMPLETION_VERSION;b[V2_BATCH_LEN+1]=waveform;
    handle_vibration_v2(V2_BATCH,p,sizeof(p));assert(ack[8]==V2_OK);
    assert(v2_upload_admission.entries[0].raw_required==waveform);
}
static void summary(uint32_t id) {
    uint8_t p[72]={0};v2_w16(p,0x1234);v2_w32(p+2,id);v2_w16(p+6,26667);v2_w32(p+8,2);
    for(unsigned i=0;i<15;i++)v2_w32(p+12+4*i,1000+i);
    handle_cmd_8c_vibration_summary(p,sizeof(p));assert(ack[1]==V2_KIND_FEATURES);
}
static void scalar_manifest(uint32_t batch_id,uint8_t captured,uint8_t status)
{
    uint8_t report[2+SCALAR_MANIFEST_LEN]={0};
    uint8_t *manifest=report+2;
    v2_w16(report,0x1234);
    manifest[0]=V2_VERSION;
    v2_w32(manifest+1,batch_id);
    manifest[5]=SCALAR_POLICY_VERSION;
    manifest[6]=SCALAR_ALL;
    manifest[7]=captured;
    uint8_t item[10]={0};
    v2_w32(item,batch_id);
    for(unsigned i=0;i<4;i++)
        v2_w32(manifest+8+4*i,scalar_value_crc(i,item));
    handle_scalar_manifest(report,sizeof(report));
    assert(ack[1]==SCALAR_ACK_KIND && ack[8]==status);
}
int main(void) {
    assert(!init_database() && !insert_new_device(mac,"test"));
    UploadTimingRecord_t t;
    batch(1,0);now_ms+=2000;summary(1);
    assert(ack[8]==V2_OK && get_upload_timing(mac,0,&t)==1 && t.samples==1 && t.last_duration_ms==2000);
    assert(strstr(timing_message,"本次=2000 ms") && !strstr(timing_message,"标量上传"));
    assert(!get_upload_timing(mac,1,&t) && !upload_owner(&v2_upload_admission,mac,1,now_ms));
    now_ms+=1000;summary(1);assert(get_upload_timing(mac,0,&t)==1 && t.samples==1);
    batch(2,1);now_ms+=100;
    scalar_manifest(2,SCALAR_ALL,V2_MISSING);
    assert(record_collection_temp(mac,2,0)>=0);
    assert(record_collection_mag(mac,2,0,0,0)>=0);
    assert(record_collection_sound(mac,2,1,0)>=0);
    assert(record_collection_sound(mac,2,0,0)>=0);
    now_ms+=200;scalar_manifest(2,SCALAR_ALL,V2_OK);
    now_ms+=300;scalar_manifest(2,SCALAR_ALL,V2_OK);
    now_ms+=6400;
    int32_t pending[3][5];for(unsigned i=0;i<15;i++)pending[i/5][i%5]=INT32_MIN;
    assert(!save_vibration_batch(mac,2,26667,2,raw,sizeof(raw),pending));
    upload_raw_seen(&v2_upload_admission,mac,2);upload_progress(&v2_upload_admission,mac,2,1,now_ms);
    summary(2);assert(ack[8]==V2_OK && get_upload_timing(mac,1,&t)==1 && t.samples==1 && t.last_duration_ms==7000);
    assert(strstr(timing_message,"本次=7000 ms") && strstr(timing_message,"磁场）=300 ms"));
    now_ms+=100;summary(2);assert(get_upload_timing(mac,1,&t)==1 && t.samples==1);
    batch(3,0);now_ms+=100;
    assert(record_collection_temp(mac,3,0)>=0);
    scalar_manifest(3,1U,V2_OK);
    now_ms+=2900;
    assert(sqlite3_exec(g_db,"CREATE TRIGGER fail_summary BEFORE UPDATE ON vibration_batches BEGIN SELECT RAISE(ABORT,'test'); END;",NULL,NULL,NULL)==SQLITE_OK);
    summary(3);assert(ack[8]==V2_STORE_FAILED && get_upload_timing(mac,0,&t)==1 && t.samples==1);
    assert(upload_owner(&v2_upload_admission,mac,3,now_ms));
    assert(sqlite3_exec(g_db,"DROP TRIGGER fail_summary;",NULL,NULL,NULL)==SQLITE_OK);
    summary(3);assert(ack[8]==V2_OK && get_upload_timing(mac,0,&t)==1 && t.samples==2);
    assert(strstr(timing_message,"本次=3000 ms") && !strstr(timing_message,"标量上传"));
    v2_gateway_remove(mac);close_database();
    puts("Actual summary timing PASS: enhanced BATCH raw-policy decoding, raw/features samples, separate summary commit, duplicate ACK and failed SQL do not train");return 0;
}
