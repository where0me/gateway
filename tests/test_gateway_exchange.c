#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GATEWAY_DATABASE_PATH "gateway-exchange-test.db"
#include "../gateway-core/src/db_manager.c"
#include "control_sync.h"
#include "gateway_exchange_wire.h"
#include "monitor_protocol.h"
#include "scalar_db.h"

static const char *mac="01:02:03:04:05:06:07:08";
static uint8_t desired[CONTROL_CONFIG_SIZE]={1};
static uint64_t clock_ms=10000U;
static unsigned replies;
static uint8_t last[GX_HEADER+GX_SAMPLING_SIZE];
static int g_uart_fd=1,g_uart_mutex,g_mqtt_store_ready;

static void pthread_mutex_lock(int *m){assert(!*m);*m=1;}
static void pthread_mutex_unlock(int *m){assert(*m);*m=0;}
static uint64_t mg_millis(void){return clock_ms++;}
static void refresh_device_activity_by_short(uint16_t address){assert(address==0x1234);}
static void channel_migration_note_node_awake(uint16_t address){assert(address==0x1234);}
static int lookup_mac_by_short(uint16_t address,char out[24])
{
    if(address!=0x1234)return 0;
    strcpy(out,mac);return 1;
}
static MonitorConfig_t load_monitor_config_for_mac(const char *m)
{
    assert(!strcmp(m,mac));
    MonitorConfig_t cfg={.auto_interval_sec=300};return cfg;
}
static int build_monitor_config_payload(uint16_t address,const MonitorConfig_t *cfg,
                                        uint8_t wire[31],char out[32])
{
    assert(cfg->auto_interval_sec==300);
    memcpy(wire+2,desired,sizeof(desired));
    return lookup_mac_by_short(address,out);
}
static int mqtt_store_policy(sqlite3 *db,const char *m,unsigned mask)
{(void)db;(void)m;(void)mask;return 0;}
static int send_hex_frame_to_uart_checked(int fd,uint8_t cmd,uint8_t len,const uint8_t *p)
{
    assert(fd==1 && cmd==GX_UART_DOWN && gx_get16(p)==0x1234 && g_uart_mutex);
    memcpy(last,p+2,len-2);replies++;return 0;
}
#include "gateway_exchange_backend.h"

static void context(uint32_t token,uint32_t batch,uint32_t revision,uint32_t hash,
                    uint8_t flags,const uint8_t *manifest)
{
    uint8_t p[2+GX_HEADER+GX_CONTEXT_SIZE+GX_SCALAR_SIZE]={0};
    gx_put16(p,0x1234);
    unsigned n=GX_CONTEXT_SIZE;
    if(manifest){memcpy(p+2+GX_HEADER+n,manifest,GX_SCALAR_SIZE);n+=GX_SCALAR_SIZE;}
    gx_header(p+2,GX_EVENT,GX_CONTEXT,0,n);
    uint8_t *b=p+2+GX_HEADER;
    gx_put32(b,token);gx_put32(b+4,batch);gx_put32(b+8,revision);gx_put32(b+12,hash);
    b[16]=flags;
    handle_gateway_exchange(p,2+GX_HEADER+n);
}
static void ready(uint32_t token,uint32_t batch,uint32_t revision,uint8_t save)
{
    uint8_t p[2+GX_HEADER+GX_READY_SIZE]={0};
    gx_put16(p,0x1234);gx_header(p+2,GX_EVENT,GX_READY,0,GX_READY_SIZE);
    uint8_t *b=p+2+GX_HEADER;
    gx_put32(b,token);gx_put32(b+4,batch);gx_put32(b+8,revision);b[12]=save;
    handle_gateway_exchange(p,sizeof(p));
}

static void final_result(uint16_t seq,uint32_t token,uint32_t batch,uint32_t revision,uint8_t result)
{
    uint8_t p[2+GX_HEADER+GX_SAMPLING_RESULT_SIZE]={0};
    gx_put16(p,0x1234);gx_header(p+2,GX_RESPONSE,GX_SAMPLING,seq,GX_SAMPLING_RESULT_SIZE);
    uint8_t *b=p+2+GX_HEADER;
    gx_put32(b,token);gx_put32(b+4,batch);b[8]=result;gx_put32(b+9,revision);
    handle_gateway_exchange(p,sizeof(p));
}

int main(void)
{
    assert(init_database()==0);
    assert(insert_new_device(mac,"sensor")==0);
    uint32_t hash=control_fingerprint(desired,sizeof(desired));
    context(10,0,0,0,0,NULL);ready(10,0,0,0);
    assert(!replies);
    assert(update_device_online(mac,"1234")==0);
    assert(confirm_device_join(mac,0x1234)>0);
    assert(save_device_interview(mac,"{\"state\":\"ready\"}")==0);
    assert(update_device_link_telemetry(mac,155,-45,4)==0);
    assert(request_device_admission(mac)==1);
    assert(authorize_device(mac)>0);

    ready(10,0,0,0);assert(!replies);
    context(10,0,0,0,GX_CONTEXT_FINAL,NULL);ready(10,0,0,0);
    assert(replies==1 && gx_get16(last+4)==1 &&
           gx_get16(last+GX_HEADER+13)==GX_COMMANDS_PENDING);
    context(10,0,0,0,GX_CONTEXT_FINAL|GX_CONTEXT_COMMANDS,NULL);ready(10,0,0,0);
    gx_retry_tick();
    assert(replies==2 && last[1]==GX_COMMAND && last[2]==GX_SAMPLING &&
           !memcmp(last+GX_HEADER+GX_KEY_SIZE,desired,sizeof(desired)));
    uint16_t request=gx_get16(last+4);
    uint8_t original[GX_COMMAND_FRAME_SIZE];memcpy(original,last,sizeof(original));
    clock_ms+=2100;gx_retry_tick();
    assert(replies==3 && !memcmp(original,last,sizeof(original)));
    final_result(request,10,0,0,0);assert(replies==3); /* Wrong resulting revision. */
    final_result(request,10,0,0,GX_SAVE_FAILED);
    assert(replies==3); /* Save failure never gets permission. */
    clock_ms+=2100;gx_retry_tick();
    assert(replies==4 && gx_newer(gx_get16(last+4),request) && last[1]==GX_COMMAND);
    request=gx_get16(last+4);
    close_database();memset(gx_contexts,0,sizeof(gx_contexts));
    assert(init_database()==0);
    final_result(request,10,0,1,0); /* No new READY required after restart. */
    assert(replies==5 && gx_get16(last+GX_HEADER+13)==GX_SLEEP_ALLOWED &&
           gx_get32(last+GX_HEADER+8)==1);
    uint16_t permit=gx_get16(last+4);
    desired[0]=2; /* A late replay after permission must not open a new command. */
    final_result(request,10,0,1,0);
    assert(replies==6 && gx_get16(last+GX_HEADER+13)==GX_SLEEP_ALLOWED &&
           gx_newer(gx_get16(last+4),permit));
    desired[0]=1;
    assert(gx_decision_valid(3,2,0,GX_SLEEP_ALLOWED));
    assert(!gx_decision_valid(2,3,0,GX_SLEEP_ALLOWED));
    assert(!gx_decision_valid(4,3,0,3));
    assert(!gx_decision_valid(4,3,GX_NOT_READY,GX_SLEEP_ALLOWED));
    assert(gx_decision_valid(4,3,GX_SAVE_FAILED,0));
    uint8_t status[CONTROL_FINAL_SIZE]={CONTROL_STATUS_VERSION,CONTROL_SAVE_FAILED};
    assert(control_status_valid(status,CONTROL_STATUS_SIZE));
    assert(control_final_status_valid(status,CONTROL_FINAL_SIZE));
    status[1]|=CONTROL_APPLIED;
    assert(!control_final_status_valid(status,CONTROL_FINAL_SIZE));

    /* A policy edit while the command is in flight continues in the same round. */
    context(15,0,0,0,GX_CONTEXT_FINAL|GX_CONTEXT_COMMANDS,NULL);ready(15,0,0,0);
    clock_ms+=300;gx_retry_tick();
    request=gx_get16(last+4);
    unsigned before=replies;
    desired[0]=3;
    final_result(request,15,0,1,0);
    assert(replies==before);
    clock_ms+=300;gx_retry_tick();
    uint16_t next=gx_get16(last+4);
    assert(replies==before+1 && last[1]==GX_COMMAND && gx_newer(next,request));
    assert(gx_get32(last+GX_HEADER+8)==1 && last[GX_HEADER+GX_KEY_SIZE]==3);
    final_result(request,15,0,1,0);
    assert(replies==before+1);
    final_result(next,15,0,2,0);
    assert(replies==before+2 && gx_get16(last+GX_HEADER+13)==GX_SLEEP_ALLOWED &&
           gx_get32(last+GX_HEADER+8)==2);
    desired[0]=1;

    /* Previous firmware keeps using a new READY after applying the policy. */
    context(11,0,0,0,GX_CONTEXT_COMMANDS,NULL);ready(11,0,0,0);
    assert(last[1]==GX_COMMAND && gx_get16(last+4)==1);
    context(11,0,1,hash,0,NULL);ready(11,0,1,0);
    assert(gx_get16(last+GX_HEADER+13)==GX_SLEEP_ALLOWED);

    context(20,11,0,hash,0,NULL);ready(20,11,0,0);
    assert(last[GX_HEADER+12]==GX_NOT_READY && !gx_get16(last+GX_HEADER+13));
    uint8_t manifest[SCALAR_MANIFEST_LEN]={2},temp[6]={0};
    gx_put32(manifest+1,12);manifest[5]=SCALAR_POLICY_VERSION;manifest[6]=manifest[7]=1;
    gx_put32(temp,12);gx_put16(temp+4,2500);gx_put32(manifest+8,scalar_value_crc(0,temp));
    context(21,12,0,hash,GX_CONTEXT_SCALAR,manifest);ready(21,12,0,0);
    assert(last[GX_HEADER+12]==GX_NOT_READY);
    assert(record_collection_temp(mac,12,25.0f)==1);
    ready(21,12,0,0);
    assert(last[GX_HEADER+12]==0 && gx_get16(last+GX_HEADER+13)==GX_SLEEP_ALLOWED);
    for(unsigned raw_required=0;raw_required<2U;raw_required++) {
        uint32_t batch=30U+raw_required;
        uint8_t raw[72]={0},descriptor[V2_BATCH_LEN]={2},receipt_manifest[SCALAR_MANIFEST_LEN]={2};
        int32_t features[3][5]={{0}};
        gx_put32(descriptor+1,batch);gx_put16(descriptor+5,26667);
        gx_put32(descriptor+7,12);gx_put16(descriptor+11,V2_POINTS);
        gx_put32(descriptor+13,v2_crc(raw,sizeof(raw)));
        gx_put32(receipt_manifest+1,batch);receipt_manifest[5]=SCALAR_POLICY_VERSION;
        assert(remember_upload_batch(mac,descriptor,raw_required)==0);
        assert(remember_upload_manifest(mac,batch,receipt_manifest)==0);
        assert(save_vibration_batch(mac,batch,26667,12,raw_required?raw:NULL,
                                    raw_required?sizeof(raw):0,features)==0);
        context(batch,batch,0,hash,0,NULL);ready(batch,batch,0,0);
        assert(last[GX_HEADER+12]==0 && gx_get16(last+GX_HEADER+13)==GX_SLEEP_ALLOWED);
    }
    close_database();
    puts("Gateway exchange PASS: final response, same-sequence replay, new-sequence save retry, restart, multi-command revision, legacy READY, and durable batch validation");
}
