#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#define GATEWAY_DATABASE_PATH "out/vibration-tests/join-test.db"
#include "../gateway-core/src/db_manager.c"
#include "cJSON.h"
/* Single-threaded deterministic harness, actual admission/HTTP/parser code. */
static int g_pending_mutex,g_uart_mutex,g_uart_fd=1,g_scan_state;
static uint64_t now=100;
static uint64_t signal_now_ms(void){return now;}
static void pthread_mutex_lock(int *m){assert(!*m);*m=1;}
static void pthread_mutex_unlock(int *m){assert(*m);*m=0;}
static unsigned sent, last_cmd;
static void send_hex_frame_to_uart(int fd,unsigned char cmd,unsigned char len,const unsigned char *p)
{(void)len;(void)p;assert(fd>=0);assert(cmd!=0x02);sent++;last_cmd=cmd;}
static int send_hex_frame_to_uart_checked(int fd,unsigned char cmd,unsigned char len,const unsigned char *p)
{send_hex_frame_to_uart(fd,cmd,len,p);return 0;}
static void gateway_log_event(const char *l,const char *s,const char *e,const char *mac,int addr,int value,const char *fmt,...)
{(void)l;(void)s;(void)e;(void)mac;(void)addr;(void)value;(void)fmt;}
struct mg_str {const char *buf;size_t len;};
struct mg_connection {int unused;};
struct mg_http_message {struct mg_str method,body;};
static struct mg_str mg_str(const char *s){return (struct mg_str){s,strlen(s)};}
static int mg_strcmp(struct mg_str a,struct mg_str b){return a.len!=b.len || memcmp(a.buf,b.buf,a.len);}
static int code;static char reply[1024];
static void mg_http_reply(struct mg_connection *c,int status,const char *h,const char *fmt,...)
{(void)c;(void)h;code=status;va_list ap;va_start(ap,fmt);vsnprintf(reply,sizeof(reply),fmt,ap);va_end(ap);}
/* This harness isolates radio state and identity parsing. Durable close cleanup
 * is exercised with the real implementation by test_manual_admission.c. */
static int cancel_pending_admissions(void){return 0;}
#include "gateway_join.h"
#include "gateway_admission.h"
static cJSON *response(void){cJSON *r=cJSON_Parse(reply);assert(r);return r;}
static int boolean(const char *key){cJSON *r=response();int b=cJSON_IsTrue(cJSON_GetObjectItem(r,key));cJSON_Delete(r);return b;}
int main(void)
{
    remove(GATEWAY_DATABASE_PATH);assert(init_database()==0);
    struct mg_connection c={0};struct mg_http_message get={.method=mg_str("GET")},post={.method=mg_str("POST")};
    join_api(&c,&get,-1);assert(code==200 && !boolean("known"));
    join_api(&c,&post,1);assert(code==202 && last_cmd==3 && g_scan_state==0);
    join_api(&c,&post,1);assert(code==409);
    unsigned char open[]={1,254}; join_observe(open,2);
    join_api(&c,&get,-1);assert(boolean("known") && boolean("active") && !boolean("pending"));
    const unsigned char node[]={1,2,3,4,5,6,7,8,0x34,0x12};
    const char *mac="01:02:03:04:05:06:07:08";
    accept_verified_scan_node(node,0,-128);assert(is_device_in_db(mac));
    DeviceRecord_t *visible=NULL;
    assert(get_confirmed_devices(&visible)==0);free(visible);
    int enabled=-1;assert(get_device_monitoring(mac,&enabled) && !enabled);
    assert(update_device_monitoring(mac,1)==0);
    accept_verified_scan_node(node,0,-128);assert(get_device_monitoring(mac,&enabled) && enabled);
    /* Duplicate model replies do not write again or overwrite user names. */
    char model[64],custom[128],display[128];
    assert(update_device_custom_name(mac,"Workshop sensor")==0);
    int changes=sqlite3_total_changes(g_db);
    assert(update_device_model_name(mac,"HY-Sensor")==0);
    assert(sqlite3_total_changes(g_db)==changes+1);
    changes=sqlite3_total_changes(g_db);
    assert(update_device_model_name(mac,"HY-Sensor")==0);
    assert(sqlite3_total_changes(g_db)==changes);
    assert(update_device_model_name(mac,"HY-Sensor-v4")==0);
    assert(sqlite3_total_changes(g_db)==changes+1);
    assert(get_device_name_info(mac,model,sizeof(model),custom,sizeof(custom),display,sizeof(display)));
    assert(!strcmp(model,"HY-Sensor-v4") && !strcmp(custom,"Workshop sensor") && !strcmp(display,custom));
    join_api(&c,&post,0);assert(code==202 && last_cmd==5 && g_scan_state==1);
    unsigned char closed[]={0,0};join_observe(closed,2);
    join_api(&c,&get,-1);assert(!boolean("active") && boolean("known"));
    assert(is_device_in_db(mac) && get_device_monitoring(mac,&enabled) && enabled);
    /* A late, still-unapproved announcement is rejected after manual close. */
    unsigned char late[]={11,12,13,14,15,16,17,18,0x35,0x12};
    accept_verified_scan_node(late,0,-128);assert(!is_device_in_db("0B:0C:0D:0E:0F:10:11:12"));
    assert(get_revoked_node_by_mac("0B:0C:0D:0E:0F:10:11:12",NULL));
    join_api(&c,&post,1);join_observe(open,2);
    assert(clear_revoked_node("0B:0C:0D:0E:0F:10:11:12")==0);
    accept_verified_scan_node(late,0,-128);assert(is_device_in_db("0B:0C:0D:0E:0F:10:11:12"));
    /* Address reuse moves routing, never deletes the old IEEE or its switch. */
    const char *other="0B:0C:0D:0E:0F:10:11:12";
    char resolved[32]; uint16_t address;
    assert(update_device_online(other,"1234")==0);
    assert(get_mac_by_short_addr("1234",resolved) && !strcmp(resolved,other));
    assert(!get_device_short_addr(mac,&address));
    assert(is_device_in_db(mac) && get_device_monitoring(mac,&enabled) && enabled);
    assert(update_device_online("FF:FF:FF:FF:FF:FF:FF:FF","1234")==0);
    assert(get_mac_by_short_addr("1234",resolved) && !strcmp(resolved,other));
    assert(update_device_online(other,"1235")==0 && update_device_online(mac,"1234")==0);
    unsigned char bad[10]={0};accept_verified_scan_node(bad,0,0);
    assert(!is_device_in_db("00:00:00:00:00:00:00:00"));
    unsigned char desc[]={1,2,3,4,5,6,7,8,0x34,0x12,1,1,4,1,3,0,0,0,0x10,0xfc,0x11,0xfc};
    handle_interview(0xA2,desc,sizeof(desc));
    unsigned char proto[]={1,2,3,4,5,6,7,8,0x34,0x12,3,0,31,0};
    handle_interview(0xA3,proto,sizeof(proto));
    /* Names, cached signals, descriptors and protocol reads are not enough. */
    assert(get_confirmed_devices(&visible)==0);free(visible);
    unsigned char ready[]={1,2,3,4,5,6,7,8,0x34,0x12,3,1};
    ready[8]=0x77;handle_interview(0xA2,ready,sizeof(ready));
    assert(get_confirmed_devices(&visible)==0);free(visible);
    ready[8]=0x34;handle_interview(0xA2,ready,sizeof(ready));
    assert(get_confirmed_devices(&visible)==0);free(visible);
    assert(update_device_link_telemetry(mac,150,-45,4)==0);
    assert(authorize_device(mac)==1);
    assert(get_confirmed_devices(&visible)==1 && !strcmp(visible[0].mac,mac));free(visible);
    assert(update_device_offline(mac)==0);
    assert(get_confirmed_devices(&visible)==1);free(visible);
    assert(update_device_online(mac,"1234")==0);
    char *before=load_device_interview(mac);assert(before && strstr(before,"64528"));
    desc[14]=255;handle_interview(0xA2,desc,sizeof(desc));
    proto[8]=0x77;handle_interview(0xA3,proto,sizeof(proto));
    char *after=load_device_interview(mac);assert(after && !strcmp(before,after));free(after);
    close_database();assert(init_database()==0);after=load_device_interview(mac);assert(after && !strcmp(before,after));free(before);free(after);
    post.body=mg_str("{\"mac\":\"01:02:03:04:05:06:07:08\"}");
    interview_retry_api(&c,&post);assert(code==202 && last_cmd==0x14);
    now+=9000;join_api(&c,&get,-1);assert(!boolean("known"));
    join_tick();assert(last_cmd==0x13);
    g_uart_fd=-1;join_api(&c,&post,1);assert(code==503);
    assert(delete_device(mac)==0);assert(!load_device_interview(mac));
    close_database();assert(remove(GATEWAY_DATABASE_PATH)==0);
    puts("Candidate registration, model reply deduplication/custom names, late-close rejection, permit confirmation/staleness, metadata validation/persistence PASS");
}
