#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GATEWAY_DATABASE_PATH "admission.db"
#include "../gateway-core/src/db_manager.c"
#include "cJSON.h"
static int g_pending_mutex,g_uart_mutex,g_uart_fd=1,g_scan_state;
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
static uint64_t now=100;
static uint64_t signal_now_ms(void){return now;}
static void pthread_mutex_lock(int *m){assert(!*m);*m=1;}
static void pthread_mutex_unlock(int *m){assert(*m);*m=0;}
static unsigned sends,command,pushes,leaves;
static unsigned char probe_request[4];
static unsigned char grant_request[6];
static unsigned grants;
static int probe_write_failed;
static void send_hex_frame_to_uart(int fd,unsigned char cmd,unsigned char len,const unsigned char *p)
{assert(fd>=0);sends++;command=cmd;if(cmd==0x11){assert(len==8 && p);leaves++;}
 if(cmd==0x10){assert(len==4);memcpy(probe_request,p,4);}
 if(cmd==GW_CMD_ADMISSION_GRANT){assert(len==6);memcpy(grant_request,p,6);grants++;}}
static int send_hex_frame_to_uart_checked(int fd,unsigned char cmd,unsigned char len,const unsigned char *p)
{if(probe_write_failed)return -1;send_hex_frame_to_uart(fd,cmd,len,p);return 0;}
static void gateway_log_event(const char *a,const char *b,const char *c,const char *d,int e,int f,const char *fmt,...)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)fmt;}
struct mg_str {const char *buf;size_t len;};
struct mg_connection {int unused;};
struct mg_http_message {struct mg_str method,body;};
static struct mg_str mg_str(const char *s){return (struct mg_str){s,strlen(s)};}
static int mg_strcmp(struct mg_str a,struct mg_str b){return a.len!=b.len || memcmp(a.buf,b.buf,a.len);}
static int code;static char reply[2048];
static void mg_http_reply(struct mg_connection *c,int status,const char *h,const char *fmt,...)
{(void)c;(void)h;code=status;va_list ap;va_start(ap,fmt);vsnprintf(reply,sizeof(reply),fmt,ap);va_end(ap);}
static int mac_string_to_bytes(const char *s,unsigned char p[8])
{
    unsigned v[8];if(strlen(s)!=23 || sscanf(s,"%x:%x:%x:%x:%x:%x:%x:%x",v,v+1,v+2,v+3,v+4,v+5,v+6,v+7)!=8)return -1;
    for(unsigned i=0;i<8;i++)p[i]=(unsigned char)v[i];
    return 0;
}
static int send_monitor_control(uint16_t address,unsigned char op,unsigned char enabled)
{assert(address && op==MONITOR_OP_AUTHORIZE && !enabled);pushes++;return 1;}
static int cancel_pending_admissions(void);
#include "gateway_join.h"
#include "admission_probe.h"
#include "gateway_admission.h"
static int mg_random(void *p,size_t len){memset(p,0x10,len);return 1;}
static uint16_t read_le16(const unsigned char *p){return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t read_le32(const unsigned char *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void write_le16(unsigned char *p,uint16_t v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void write_le32(unsigned char *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
#include "admission_delivery.h"
#include "manual_admission_api.h"
static int pending_count(void)
{cJSON *list=cJSON_Parse(reply);assert(cJSON_IsArray(list));int n=cJSON_GetArraySize(list);cJSON_Delete(list);return n;}
static int formal_count(void){DeviceRecord_t *r=NULL;int n=get_confirmed_devices(&r);free(r);return n;}
static void probe_time(uint64_t value)
{now=value;unsigned char open[]={1,254};join_observe(open,2);}
static void reply_to_probe(void)
{unsigned char response[6];memcpy(response,probe_request,4);response[4]=(unsigned char)-42;response[5]=174;admission_probe_reply(response,6);}
int main(void)
{
    remove(GATEWAY_DATABASE_PATH);assert(!init_database());
    admission_delivery_capabilities(0);
    const char *mac="01:02:03:04:05:06:07:08";
    const unsigned char announce[]={1,2,3,4,5,6,7,8,0x34,0x12};
    struct mg_connection c={0};struct mg_http_message get={.method=mg_str("GET")},post={.method=mg_str("POST"),.body=mg_str("{\"mac\":\"01:02:03:04:05:06:07:08\"}")};
    join_api(&c,&post,1);assert(command==3);
    unsigned char open[]={1,254};join_observe(open,2);
    accept_verified_scan_node(announce,255,-10);assert(!formal_count());
    confirm_visible_node(mac,0x1234,"node status");assert(!formal_count() && !device_is_authorized(mac));
    manual_admission_api(&c,&get,0);assert(code==200 && !pending_count());
    manual_admission_api(&c,&post,1);assert(code==409 && !pushes);
    manual_admission_tick();assert(command==4); /* missing model is retried */
    assert(!update_device_model_name(mac,"HY-Sensor"));
    assert(!save_device_interview(mac,"{\"state\":\"ready\"}"));
    manual_admission_api(&c,&get,0);assert(!pending_count()); /* no fresh signal */
    assert(!update_device_link_telemetry(mac,156,-47,4));
    unsigned before_first_list=sends;
    admission_probe_tick();assert(sends==before_first_list); /* No probe before first listing, even when ready. */
    manual_admission_api(&c,&get,0);assert(pending_count()==1);
    assert(admission_probe_visible(mac) && !admission_probe_find(mac)->token && !admission_probe_find(mac)->deadline);
    assert(admission_probe_find(mac)->due>=now+3000 && admission_probe_find(mac)->due<=now+10000);
    struct mg_http_message refresh={.method=mg_str("POST")};
    probe_write_failed=1;manual_admission_refresh_api(&c,&refresh);
    assert(!admission_probe_find(mac)->deadline && !admission_probe_find(mac)->misses);
    probe_write_failed=0;probe_time(now+151);
    manual_admission_refresh_api(&c,&refresh);assert(code==202 && command==0x10);
    unsigned char bad[6];memcpy(bad,probe_request,4);bad[2]^=1;bad[4]=(unsigned char)-42;bad[5]=174;
    admission_probe_reply(bad,6);assert(admission_probe_find(mac)->deadline); /* Wrong token cannot finish the probe. */
    admission_probe_reply(bad,5);assert(admission_probe_find(mac)->deadline);
    reply_to_probe();assert(admission_probe_visible(mac));
    uint16_t first_token=admission_probe_find(mac)->token;
    /* Three consecutive no-replies hide the candidate without deleting it.
     * An unrelated heartbeat must not reset this count. */
    for(unsigned i=0;i<3;i++) {
        AdmissionProbe *p=admission_probe_find(mac);
        probe_time(now+151);manual_admission_refresh_api(&c,&refresh);assert(p->deadline);
        uint64_t deadline=p->deadline;probe_time(deadline);admission_probe_tick();
        assert(p->misses==i+1 && !p->deadline && p->due>=now+3000 && p->due<=now+10000);
        assert(!update_device_link_telemetry(mac,200,-30,4));
        assert(admission_probe_visible(mac)==(i<2));
        manual_admission_api(&c,&get,0);assert(pending_count()==(i<2)); /* GET cannot revive hidden nodes. */
    }
    assert(is_device_in_db(mac) && !get_revoked_node_by_mac(mac,NULL));
    manual_admission_api(&c,&get,0);assert(!pending_count());
    manual_admission_api(&c,&post,1);assert(code==409 && !pushes);
    memcpy(bad,probe_request,4);bad[2]=(unsigned char)first_token;bad[3]=(unsigned char)(first_token>>8);
    admission_probe_reply(bad,6);assert(!admission_probe_visible(mac));
    /* Refresh probes hidden nodes before their next random retry is due. */
    probe_time(now+151);assert(now<admission_probe_find(mac)->due);
    manual_admission_refresh_api(&c,&refresh);
    assert(code==202 && admission_probe_find(mac)->deadline && !admission_probe_visible(mac));
    reply_to_probe();assert(admission_probe_visible(mac) && !admission_probe_find(mac)->misses);
    /* Short-address changes invalidate the old proof and late old-address replies. */
    assert(!update_device_online(mac,"1235"));assert(!admission_probe_visible(mac));
    admission_probe_sync();reply_to_probe();assert(!admission_probe_visible(mac));
    manual_admission_api(&c,&get,0);assert(pending_count()==1); /* New address gets ordinary first-listing checks. */
    probe_time(now+151);manual_admission_refresh_api(&c,&refresh);assert(probe_request[0]==0x35);
    reply_to_probe();assert(admission_probe_visible(mac));
    assert(!update_device_online(mac,"1234"));admission_probe_sync();
    manual_admission_api(&c,&get,0);assert(pending_count()==1);
    probe_time(now+151);manual_admission_refresh_api(&c,&refresh);reply_to_probe();
    manual_admission_api(&c,&get,0);assert(pending_count()==1 && !formal_count());
    cJSON *list=cJSON_Parse(reply),*item=cJSON_GetArrayItem(list,0);
    assert(cJSON_GetArraySize(item)==5 && cJSON_GetObjectItem(item,"name") && cJSON_GetObjectItem(item,"model_name"));cJSON_Delete(list);
    DeviceRecord_t *snapshot=NULL;assert(get_all_devices(&snapshot)==1);
    assert(snapshot[0].lqi==174 && snapshot[0].rssi==-42 && snapshot[0].power_source==4);free(snapshot);
    manual_admission_api(&c,&post,1);assert(code==202 && pushes==1 && !formal_count());
    assert(device_admission_requested(mac) && !device_is_authorized(mac));
    close_database();assert(!init_database());assert(device_admission_requested(mac));
    assert(authorize_device(mac)==1 && formal_count()==1 && !device_admission_requested(mac));
    manual_admission_api(&c,&post,1);assert(code==200 && pushes==1); /* no duplicate capture */
    manual_admission_api(&c,&get,0);assert(!pending_count());
    admission_probe_sync();assert(!admission_probe_find(mac)); /* Approved nodes are never probed here. */
    const unsigned char b[]={11,12,13,14,15,16,17,18,0x35,0x12},late[]={21,22,23,24,25,26,27,28,0x36,0x12};
    const char *bmac="0B:0C:0D:0E:0F:10:11:12",*late_mac="15:16:17:18:19:1A:1B:1C";
    accept_verified_scan_node(b,0,-128); /* Includes undiscovered/no-name candidate. */
    const unsigned char ready_candidate[]={31,32,33,34,35,36,37,38,0x37,0x12};
    const char *ready_mac="1F:20:21:22:23:24:25:26";
    accept_verified_scan_node(ready_candidate,0,-128);
    assert(confirm_device_join(ready_mac,0x1237)==1);
    assert(!update_device_model_name(ready_mac,"HY-Sensor"));
    assert(!save_device_interview(ready_mac,"{\"state\":\"ready\"}"));
    assert(!update_device_link_telemetry(ready_mac,150,-45,4));
    manual_admission_api(&c,&get,0);assert(pending_count()==1);
    admission_probe_sync();probe_time(now+151);
    manual_admission_refresh_api(&c,&refresh);assert(command==0x10 && probe_request[0]==0x37);reply_to_probe();
    manual_admission_api(&c,&get,0);assert(pending_count()==1);
    join_api(&c,&post,0);assert(code==202 && command==0x11 && leaves==2);
    unsigned char closed[]={0,0};join_observe(closed,2);
    assert(!is_device_in_db(bmac) && get_revoked_node_by_mac(bmac,NULL));
    assert(!is_device_in_db(ready_mac) && get_revoked_node_by_mac(ready_mac,NULL));
    RevokedNodeRecord_t pending_leave;
    assert(get_revoked_node_by_mac(ready_mac,&pending_leave) && pending_leave.last_leave_at==0);
    assert(device_is_authorized(mac) && formal_count()==1 && cancel_unapproved_device(mac,0x1234)==1);
    manual_admission_api(&c,&get,0);assert(!pending_count());
    manual_admission_api(&c,&post,1);assert(code==409 && pushes==1); /* Stale browser cannot authorize after stop. */
    accept_verified_scan_node(late,0,-128);assert(!is_device_in_db(late_mac) && get_revoked_node_by_mac(late_mac,NULL) && leaves==3);
    close_database();assert(!init_database());assert(get_revoked_node_by_mac(bmac,NULL));
    join_api(&c,&post,1);assert(command==3);join_observe(open,2);
    assert(clear_revoked_node(bmac)==0); /* Explicit coordinator cleanup confirmation. */
    accept_verified_scan_node(b,0,-128);assert(is_device_in_db(bmac) && !device_is_authorized(bmac));
    g_uart_fd=-1;join_api(&c,&post,0);assert(code==503 && !is_device_in_db(bmac) && get_revoked_node_by_mac(bmac,NULL));g_uart_fd=1;
    close_database();assert(!init_database());assert(device_is_authorized(mac) && formal_count()==1);
    /* Existing installed nodes keep authorization and begin monitoring after upgrade. */
    assert(sqlite3_exec(g_db,"ALTER TABLE devices DROP COLUMN user_authorized; UPDATE devices SET monitoring_enabled=0;",0,0,0)==SQLITE_OK);
    close_database();assert(!init_database());int on=0;
    assert(device_is_authorized(mac) && get_device_monitoring(mac,&on) && on);
    admission_probe_clear();
    /* Fresh instance: first listing and grant both work before any probe reply. */
    close_database();remove(GATEWAY_DATABASE_PATH);assert(!init_database());
    probe_time(now+6000);join_api(&c,&post,1);join_observe(open,2);
    accept_verified_scan_node(announce,0,-128);assert(confirm_device_join(mac,0x1234)==1);
    assert(!update_device_model_name(mac,"HY-Sensor"));assert(!save_device_interview(mac,"{\"state\":\"ready\"}"));
    assert(!update_device_link_telemetry(mac,156,-47,4));
    before_first_list=sends;admission_probe_refresh();admission_probe_tick();assert(sends==before_first_list);
    manual_admission_api(&c,&get,0);assert(pending_count()==1 && !admission_probe_find(mac)->token);
    unsigned old_pushes=pushes;manual_admission_api(&c,&post,1);assert(code==202 && pushes==old_pushes+1 && !formal_count());
    probe_time(now+5001);manual_admission_tick();assert(pushes==old_pushes+2 && !formal_count());
    assert(admission_delivery_apply(mac)==1 && formal_count()==1);

    /* Tagged grants serialize attempts and only node application finishes admission. */
    assert(sqlite3_exec(g_db,"UPDATE devices SET user_authorized=0,sampling_admission=1",
                        NULL,NULL,NULL)==SQLITE_OK);
    admission_delivery_capabilities(GW_CAP_ADMISSION_GRANT);
    send_admission_grant(mac,1);
    assert(grants==1 && read_le16(grant_request)==0x1234);
    uint32_t first=read_le32(grant_request+2);
    unsigned char result[GW_ADMISSION_RESULT_SIZE]={0x34,0x12,GW_CMD_MONITOR_CONTROL,0,42};
    result[9]=GW_ADMISSION_AF_RESULT;
    write_le32(result+5,first);
    now+=5001;send_admission_grant(mac,0);assert(grants==1);
    admission_delivery_result(result);
    assert(!device_is_authorized(mac));
    now+=ADMISSION_APPLY_WAIT_MS-1;send_admission_grant(mac,0);assert(grants==1);
    ++now;send_admission_grant(mac,0);assert(grants==2);
    uint32_t second=read_le32(grant_request+2);
    assert(second!=first);
    result[3]=0xf0;
    admission_delivery_result(result); /* A result for attempt 1 cannot change attempt 2. */
    now+=ADMISSION_RETRY_DELAY_MS;send_admission_grant(mac,0);assert(grants==2);
    write_le32(result+5,second);
    admission_delivery_result(result);
    now+=ADMISSION_RETRY_DELAY_MS-1;send_admission_grant(mac,0);assert(grants==2);
    ++now;send_admission_grant(mac,0);assert(grants==3);
    write_le32(result+5,read_le32(grant_request+2));
    assert(admission_delivery_apply(mac)==1);
    admission_delivery_result(result); /* Application status precedes the late AF failure. */
    now+=ADMISSION_AF_WAIT_MS;send_admission_grant(mac,0);
    assert(grants==3 && device_is_authorized(mac) && formal_count()==1);
    admission_probe_clear();close_database();
    puts("Manual admission PASS: first listing/grant before any probe, no pre-list probes, random subsequent probes, 3 misses hide without GET revival, token recovery, close cleanup and persistence");
}
