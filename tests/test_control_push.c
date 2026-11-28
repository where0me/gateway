#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#define GATEWAY_DATABASE_PATH ":memory:"
#include "../gateway-core/src/db_manager.c"
#include "control_sync.h"
#include "monitor_protocol.h"
#include "cJSON.h"
static uint64_t clock_ms=10000;
static uint64_t mg_millis(void){return clock_ms;}
#define CHANNEL_OPERATION_PENDING 1
static pthread_mutex_t g_channel_operation_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct {int phase; uint8_t node_count,target_channel; struct {char mac[32];} nodes[8];} g_channel_operation;
static MonitorConfig_t policy={.auto_interval_sec=300,.poll_interval_sec=5,
    .vibration_rate_hz=26667,.digital_mic_rate_hz=46875,.analog_mic_rate_hz=256000,
    .vibration_window_ms=500,.digital_mic_window_ms=1000,.analog_mic_window_ms=1000};
static int lookup_mac_by_short(uint16_t,char[24]);
static MonitorConfig_t load_monitor_config_for_mac(const char *mac){(void)mac;return policy;}
static int build_monitor_config_payload(uint16_t,const MonitorConfig_t *,unsigned char[31],char[32]);
static int control_delivery(const char *,uint32_t,int);
static unsigned window_sends,schedule_sends,monitor_sends;
static int send_monitor_config_scoped(uint16_t,const MonitorConfig_t *);
static void handle_control_status(const unsigned char *,unsigned);
static int admission_delivery_apply(const char *mac){return authorize_device(mac);}
void handle_cmd_99_config_query(unsigned char *,unsigned char);
static unsigned char last_payload[40];
static pthread_mutex_t g_uart_mutex=PTHREAD_MUTEX_INITIALIZER;
static int g_uart_fd=1;
static int g_mqtt_store_ready;
static int mqtt_store_policy(sqlite3 *db,const char *mac,unsigned int mask)
{(void)db;(void)mac;(void)mask;return 0;}
static void config_diag_tx(uint64_t now,const uint8_t *payload,unsigned n,const char *mac,int available)
{(void)now;(void)payload;(void)mac;(void)available;assert(n==31);}
static void send_hex_frame_to_uart(int fd,uint8_t cmd,uint8_t length,const unsigned char *p)
{
    assert(fd==1);
    memcpy(last_payload,p,length);
    if (cmd==CONTROL_RESUME_SCHEDULE_CMD) {
        assert(length==2U+CONTROL_RESUME_SCHEDULE_SIZE);schedule_sends++;
    } else {
        assert(cmd==CONTROL_CONFIG_SYNC_CMD && length==32 && p[31]==CONTROL_DELIVERY_WINDOW);
        window_sends++;
    }
}
static int send_monitor_control(uint16_t address,unsigned char operation,unsigned char enabled)
{
    assert(address==0x1234 && operation==MONITOR_OP_SET_ENABLED && enabled<=1);
    monitor_sends++;
    return 1;
}
static void config_diag_print(uint64_t n,const char *fmt,...){(void)n;(void)fmt;}
static uint16_t read_le16(const unsigned char *p){return p[0]|((uint16_t)p[1]<<8);}
static void write_le16(unsigned char *p,uint16_t v){p[0]=v;p[1]=v>>8;}
static void write_le32(unsigned char *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(i*8);}
struct mg_connection {int unused;};
struct mg_http_message {const char *json;};
static int reply_status;
static void mg_http_reply(struct mg_connection *c,int code,const char *headers,const char *fmt,...)
{(void)c;(void)headers;(void)fmt;reply_status=code;}
static int parse_device_mac(struct mg_http_message *hm,char *mac,cJSON **root)
{
    *root=cJSON_Parse(hm->json);cJSON *m=cJSON_GetObjectItem(*root,"mac");
    if(!cJSON_IsString(m))return 0;snprintf(mac,32,"%s",m->valuestring);return 1;
}
static void gateway_log_event(const char *level,const char *src,const char *kind,const char *mac,
    int address,long long batch,const char *fmt,...)
{(void)level;(void)src;(void)kind;(void)mac;(void)address;(void)batch;(void)fmt;}
static pthread_mutex_t g_ws_mutex=PTHREAD_MUTEX_INITIALIZER;
static char g_ws_msg_buffer[1024];
static void queue_ws_event_locked(void) {}
void api_handle_device_monitor(struct mg_connection *,struct mg_http_message *);
static void report(unsigned char p[12],uint16_t address,uint8_t flags,uint32_t hash)
{
    memset(p,0,12);write_le16(p,address);p[2]=1;p[3]=flags;
    write_le32(p+4,hash);p[8]=4;p[10]=149;p[11]=(uint8_t)-49;
}
int main(void)
{
    assert(init_database()==0);
    policy.raw_upload_every_cycles=1;policy.raw_upload_generation=1;policy.sensor_enable_mask=7;
    const char *mac="01:02:03:04:05:06:07:08";
    assert(insert_new_device(mac,"Sensor")==0);
    assert(update_device_online(mac,"1234")==0);
    unsigned char p[12];
    report(p,0x1234,CONTROL_UPLOAD_CONFIG_WINDOW,0);
    handle_control_status(p,12);
    assert(!window_sends && !schedule_sends);
    assert(save_device_interview(mac,"{\"state\":\"ready\"}")==0);
    assert(request_device_admission(mac)==1);
    report(p,0x1234,CONTROL_VALID|CONTROL_ENABLED|CONTROL_APPLIED,0);
    handle_control_status(p,12);
    assert(device_is_authorized(mac) && !window_sends);
    struct mg_connection c={0};
    struct mg_http_message hm={"{\"mac\":\"01:02:03:04:05:06:07:08\",\"enabled\":true}"};
    api_handle_device_monitor(&c,&hm);
    assert(reply_status==200 && monitor_sends==1 && !window_sends);
    policy.auto_interval_sec=600;policy.vibration_window_ms=1000;
    report(p,0x1234,CONTROL_VALID|CONTROL_ENABLED,0);
    handle_control_status(p,12);
    assert(!window_sends && !schedule_sends);
    unsigned char legacy[]={0x34,0x12,0x02};
    handle_cmd_99_config_query(legacy,sizeof(legacy));
    assert(!window_sends && !schedule_sends);
    report(p,0x1234,CONTROL_VALID|CONTROL_NEEDS_SCHEDULE|CONTROL_UPLOAD_CONFIG_WINDOW,0);
    handle_control_status(p,12);
    assert(!window_sends && schedule_sends==1 && last_payload[2]==1);
    assert(control_read_hash(last_payload+3)>0);
    assert(control_read_hash(last_payload+7)>0 && control_read_hash(last_payload+7)<=600000);
    report(p,0x1234,CONTROL_VALID|CONTROL_ENABLED|CONTROL_UPLOAD_CONFIG_WINDOW,0);
    handle_control_status(p,12);
    assert(window_sends==1 && schedule_sends==1);
    assert(read_le16(last_payload+16)==1000 && control_read_hash(last_payload+2)==600);
    uint32_t hash=control_fingerprint(last_payload+2,CONTROL_CONFIG_SIZE);
    handle_control_status(p,12);
    assert(window_sends==1);
    report(p,0x1234,CONTROL_VALID|CONTROL_ENABLED|CONTROL_APPLIED,hash);
    handle_control_status(p,12);
    clock_ms+=20000;
    report(p,0x1234,CONTROL_VALID|CONTROL_ENABLED|CONTROL_UPLOAD_CONFIG_WINDOW,hash);
    handle_control_status(p,12);
    assert(window_sends==1);
    report(p,0x9999,CONTROL_UPLOAD_CONFIG_WINDOW,0);
    handle_control_status(p,12);
    assert(window_sends==1);
    close_database();
    puts("Backend sampling policy: no unsolicited push; reconnect schedule and monitor control are separate; post-upload window PASS");
}
