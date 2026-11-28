#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#ifdef _WIN32
static struct tm *test_localtime_r(const time_t *n,struct tm *out){*out=*localtime(n);return out;}
#define localtime_r test_localtime_r
#endif
#define GATEWAY_DATABASE_PATH ":memory:"
#include "DB_IMPLEMENTATION"
#include "scalar_tx.h"
#include "vibration_tx.h"
#include "vibration_rx.h"
#include "mqtt_store.h"
#include "gateway_log.h"
#define UNUSED(x) ((void)(x))
/* SNAPSHOT_TYPE */
#define CUSTOM_APP_ZB_VALID_TEMPERATURE 1U
#define CUSTOM_APP_ZB_VALID_MAGNETIC 2U
#define CUSTOM_APP_ZB_VALID_DIGITAL_MIC 4U
#define CUSTOM_APP_ZB_VALID_ANALOG_MIC 8U
#define CUSTOM_APP_ZB_VALID_VIBRATION 16U
#define ZB_CMD_VIBRATION_SUMMARY 0x8CU
#define ZB_TX_MAX_PAYLOAD 70U
static void ZbApp_UpdateClusterSnapshot(const Custom_APP_ZigbeeSnapshot_t *s,uint32_t batch)
{(void)s;(void)batch;}
static struct {uint8_t report_has_summary_confirm;} ZbApp;
static unsigned summaries;
static void ZbApp_SendSnapshot(const Custom_APP_ZigbeeSnapshot_t *,uint32_t);
enum ZclStatusCodeT {ZCL_STATUS_SUCCESS=0,ZCL_STATUS_FAILURE=1};
static void ZbApp_Log(const char *fmt,...){(void)fmt;}
static int16_t ZbApp_Magnetic0p1uTToRaw(int16_t);
static unsigned ZbApp_ScalarPacket(const Custom_APP_ZigbeeSnapshot_t *,uint32_t,unsigned,uint8_t[10]);
static int ZbApp_PrepareScalarDelivery(uint32_t,ScalarTx *,uint32_t);
static int ZbApp_ScalarSendItem(void *,unsigned);
static int ZbApp_ScalarSendManifest(void *,const uint8_t *);
static enum ZclStatusCodeT ZbApp_SendCommand(uint8_t,const uint8_t *,uint16_t);
static Custom_APP_ZigbeeSnapshot_t flash_snapshot;
static int metadata_status=1;
static int ZigbeeVibrationStore_LoadMetadata(uint32_t batch,void *out,uint16_t size)
{assert(batch==9 && size==sizeof(flash_snapshot));if(metadata_status==1)memcpy(out,&flash_snapshot,size);return metadata_status;}
typedef int TestMutex;
static void lock(TestMutex *m){assert(!*m);*m=1;}
static void unlock(TestMutex *m){assert(*m);*m=0;}
#define pthread_mutex_lock lock
#define pthread_mutex_unlock unlock
static TestMutex g_vibration_mutex,g_uart_mutex,g_ws_mutex;
static int g_uart_fd=1,g_has_new_ws_msg;
static char g_ws_msg_buffer[512];
static uint32_t now_ms;
static int test_clock(int id,struct timespec *out){(void)id;out->tv_sec=now_ms/1000;out->tv_nsec=now_ms%1000*1000000;return 0;}
#define clock_gettime test_clock
static const char *node_mac="01:02:03:04:05:06:07:08";
static int lookup_mac_by_short(uint16_t address,char mac[24])
{if(address!=0x1234)return 0;strcpy(mac,node_mac);return 1;}
static unsigned completed;
static void publish_collection_data_ready_event(const char *mac,uint32_t batch,int done)
{assert(!strcmp(mac,node_mac) && batch==9);completed+=done!=0;}
static uint8_t ack[V2_ACK_LEN];
static unsigned ack_ready,lose_ok,lose_reply,first_missing,scalar_probes;
static void send_hex_frame_to_uart(int fd,uint8_t cmd,uint8_t len,const uint8_t *p)
{
    assert(fd==1 && cmd==V2_ACK && len==19 && v2_u16(p)==0x1234 && !ack_ready);
    if(p[3]==SCALAR_ACK_KIND) {
        scalar_probes++;
        if(p[10]==V2_MISSING && !first_missing)first_missing=(unsigned)v2_u64(p+11);
        if((lose_ok && p[10]==V2_OK) || lose_reply){lose_ok=lose_reply=0;return;}
    }
    memcpy(ack,p+2,V2_ACK_LEN);ack_ready=1;
}
static void log_scalar_capture_missing(const char *,uint16_t,uint32_t,uint8_t);
static unsigned capture_warnings;
#define GATEWAY_SCALAR_UNAVAILABLE_EVENT(mac,address,batch,unavailable) do { \
    capture_warnings++;log_scalar_capture_missing(mac,address,batch,unavailable); \
} while(0)
#define GATEWAY_SCALAR_POLICY_EVENT(mac,batch,required,unavailable) \
    mqtt_store_batch_scalar_status(g_db,mac,batch,required,unavailable)
#include "vibration_gateway.h"
static unsigned item_attempts[4],drop_items,corrupt_temp,permanent_drop,raw_packets,backpressure;
static enum ZclStatusCodeT ZbApp_SendCommand(uint8_t cmd,const uint8_t *p,uint16_t length)
{
    if(backpressure)return ZCL_STATUS_FAILURE;
    if(cmd==ZB_CMD_VIBRATION_SUMMARY) {
        assert(v2_u32(p)==9 && length==70);summaries++;return ZCL_STATUS_SUCCESS;
    }
    if(cmd==SCALAR_MANIFEST_CMD) {
        uint8_t uart[2+SCALAR_MANIFEST_LEN];v2_w16(uart,0x1234);memcpy(uart+2,p,length);
        handle_scalar_manifest(uart,length+2);return ZCL_STATUS_SUCCESS;
    }
    assert(cmd>=0x86 && cmd<=0x89 && v2_u32(p)==9);
    unsigned item=cmd-0x86;item_attempts[item]++;
    if((drop_items|permanent_drop)&(1U<<item)){drop_items&=~(1U<<item);return ZCL_STATUS_SUCCESS;}
    int16_t x=(int16_t)v2_u16(p+4);
    int result;
    if(item==0) {
        if(corrupt_temp){x++;corrupt_temp=0;}
        result=record_collection_temp(node_mac,9,x/100.0f);
    } else if(item==1)result=record_collection_mag(node_mac,9,x*0.15f,
        (int16_t)v2_u16(p+6)*0.15f,(int16_t)v2_u16(p+8)*0.15f);
    else result=record_collection_sound(node_mac,9,item==2,x/100.0f);
    assert(result>=0);return ZCL_STATUS_SUCCESS;
}
static uint8_t raw[12*6],descriptor[V2_BATCH_LEN];
static int read_raw(void *ctx,uint32_t batch,uint32_t first,unsigned count,uint8_t *out)
{(void)ctx;assert(batch==9 && first+count<=12);memcpy(out,raw+first*6,count*6);return 1;}
static ScalarTx scalar;
static UploadLease lease;
static uint32_t lease_nonce;
static int send_raw(void *ctx,uint8_t cmd,const uint8_t *p,size_t length)
{
    (void)ctx;
    if(cmd!=V2_BATCH && !(cmd==V2_QUERY && length==10)) {
        assert(scalar.done);assert(upload_lease_valid(&lease,now_ms));
    }
    if(cmd==V2_DATA)raw_packets++;
    uint8_t uart[81];v2_w16(uart,0x1234);memcpy(uart+2,p,length);handle_vibration_v2(cmd,uart,length+2);return 1;
}
static void begin(void)
{
    memset(item_attempts,0,sizeof(item_attempts));drop_items=corrupt_temp=permanent_drop=raw_packets=backpressure=0;
    ack_ready=completed=lose_ok=lose_reply=first_missing=scalar_probes=capture_warnings=0;now_ms=0;metadata_status=1;
    for(unsigned i=0;i<V2_GATEWAY_SLOTS;i++){v2_rx_reset(&v2_gateway_slots[i].rx);memset(&v2_gateway_slots[i],0,sizeof(v2_gateway_slots[i]));}
    memset(&v2_upload_admission,0,sizeof(v2_upload_admission));
    assert(sqlite3_exec(g_db,"DELETE FROM vibration_batches;DELETE FROM collection_values;",NULL,NULL,NULL)==SQLITE_OK);
    flash_snapshot=(Custom_APP_ZigbeeSnapshot_t){.temperature_centi_c=-1234,.magnetic_0p1_ut={390,-759,-215},
        .digital_mic_db_spl=0,.analog_mic_db_spl=65,.valid_mask=31,.scalar_required_mask=15,.scalar_policy_version=1};
    memset(descriptor,0,sizeof(descriptor));descriptor[0]=2;v2_w32(descriptor+1,9);v2_w16(descriptor+5,26667);
    v2_w32(descriptor+7,12);v2_w16(descriptor+11,512);
    for(unsigned i=0;i<sizeof(raw);i++)raw[i]=(uint8_t)(i*13);
    v2_w32(descriptor+13,v2_crc(raw,sizeof(raw)));
    assert(ZbApp_PrepareScalarDelivery(9,&scalar,0));
}
static int drive(unsigned milliseconds)
{
    V2Tx tx;assert(v2_tx_start(&tx,descriptor,now_ms));
    V2TxIO waveform={NULL,read_raw,send_raw};
    ScalarTxIO telemetry={&scalar,ZbApp_ScalarSendItem,ZbApp_ScalarSendManifest};
    for(unsigned end=now_ms+milliseconds;now_ms<end;now_ms+=10) {
        if(ack_ready){
            ack_ready=0;
            if(ack[1]==SCALAR_ACK_KIND)scalar_tx_receive(&scalar,ack,sizeof(ack),now_ms);
            else if(ack[1]==UPLOAD_LEASE_KIND)assert(upload_lease_receive(&lease,ack,sizeof(ack),now_ms)==1);
            else {
                v2_tx_receive(&tx,ack,sizeof(ack),now_ms);
                if(ack[1]==V2_KIND_BATCH && ack[8]==V2_OK)upload_lease_start(&lease,9,++lease_nonce,now_ms);
            }
        }
        if(tx.phase!=V2_TX_BATCH && tx.phase!=V2_TX_DONE) {
            uint8_t query[10];if(upload_lease_query(&lease,now_ms,query)){lease_nonce=lease.nonce;assert(send_raw(NULL,V2_QUERY,query,10));continue;}
            if(!upload_lease_valid(&lease,now_ms))continue;
        }
        if(tx.phase==V2_TX_PREPARE && !scalar.done) {
            int result=scalar_tx_poll(&scalar,&telemetry,now_ms);
            if(result<0)return -1;
            if(!result){tx.deadline_ms=now_ms+V2_SEND_STALL_MS;continue;}
        }
        v2_tx_poll(&tx,&waveform,now_ms);
        if(tx.phase==V2_TX_DONE)return 1;
        if(tx.phase==V2_TX_ERROR)return -1;
    }
    return 0;
}
int main(void)
{
    assert(sizeof(Custom_APP_ZigbeeSnapshot_t)==80 && offsetof(Custom_APP_ZigbeeSnapshot_t,scalar_required_mask)==78);
    assert(init_database()==0);assert(insert_new_device(node_mac,"test")==0);assert(mqtt_store_init(g_db)==0);
    assert(gateway_log_init(":memory:")==0);
#ifdef TEST_RESTART_LEASE
    begin();send_raw(NULL,V2_BATCH,descriptor,sizeof(descriptor));
    assert(ack_ready && ack[8]==V2_WAIT && v2_u32(ack+9)==35000);ack_ready=0;
    now_ms=35000;send_raw(NULL,V2_BATCH,descriptor,sizeof(descriptor));assert(ack_ready && ack[8]==V2_OK);ack_ready=0;
    uint8_t query[10]={V2_VERSION,UPLOAD_LEASE_KIND};v2_w32(query+2,9);v2_w32(query+6,12345);
    send_raw(NULL,V2_QUERY,query,sizeof(query));
    assert(ack_ready && ack[8]==V2_OK && v2_u32(ack+9)==12345 && v2_u32(ack+13)==25000);ack_ready=0;
    now_ms=95001;send_raw(NULL,V2_QUERY,query,sizeof(query));
    assert(ack_ready && ack[8]==V2_NEED_BATCH && v2_u32(ack+9)==12345);ack_ready=0;
    memset(&v2_upload_admission,0,sizeof(v2_upload_admission));v2_gateway_boot_known=0;
    send_raw(NULL,V2_BATCH,descriptor,sizeof(descriptor));assert(ack_ready && ack[8]==V2_WAIT && v2_u32(ack+9)==35000);
    gateway_log_close();close_database();puts("Actual gateway lease: startup/restart 35s guard, 25s nonce grant, 60s expiry rejects stale owner PASS");return 0;
#endif
    for(unsigned mask=0;mask<8;mask++)assert(scalar_expected_mask(mask)==(3|((mask&2)?4:0)|((mask&4)?8:0)));
    begin();drop_items=2|8;corrupt_temp=1;lose_ok=1;
    assert(drive(60000)==1 && completed==1 && raw_packets>0 && first_missing==15);
    assert(item_attempts[0]==2 && item_attempts[1]==2 && item_attempts[2]==1 && item_attempts[3]==2);
    assert(!scalar_db_missing(node_mac,scalar.manifest)); // Valid zero SPL was accepted.
    begin();flash_snapshot.scalar_required_mask=3;flash_snapshot.valid_mask=19;
    assert(ZbApp_PrepareScalarDelivery(9,&scalar,0));
    assert(drive(30000)==1 && !item_attempts[2] && !item_attempts[3]);
    sqlite3_stmt *policy_row=NULL;
    assert(sqlite3_prepare_v2(g_db,"SELECT sensor_mask FROM mqtt_batch_state WHERE batch_id=9;",-1,&policy_row,NULL)==SQLITE_OK);
    assert(sqlite3_step(policy_row)==SQLITE_ROW && sqlite3_column_int(policy_row,0)==1);sqlite3_finalize(policy_row);
    // Current desired config changed, but this immutable Flash batch still requires both microphones.
    begin();assert(update_device_monitoring(node_mac,0)==0);assert(drive(30000)==1);
    assert(item_attempts[2]==1 && item_attempts[3]==1);
    begin();permanent_drop=2;assert(drive(60000)==-1 && !raw_packets && !completed && scalar.error);
    begin();flash_snapshot.valid_mask&=~4U;assert(ZbApp_PrepareScalarDelivery(9,&scalar,0));
    lose_ok=1;assert(drive(30000)==1 && raw_packets && !item_attempts[2] && capture_warnings==1);
    assert(gateway_log_flush()==1);
    GatewayLog rows[8];int total=0;GatewayLogFilter filter={.level="WARN",.source="node",.mac=node_mac,.limit=8};
    assert(gateway_log_query(&filter,rows,&total)==1 && total==1);
    assert(!strcmp(rows[0].event,"sensor_capture_missing") && rows[0].batch==9 && rows[0].short_addr==0x1234);
    assert(strstr(rows[0].message,"数字麦克风声级") && strstr(rows[0].message,"继续"));
    // Every acquisition failure is skippable; delivery loss of other values is not.
    for(unsigned absent=1;absent<=15;absent++) {
        begin();flash_snapshot.valid_mask&=(uint8_t)~absent;
        assert(ZbApp_PrepareScalarDelivery(9,&scalar,0));drop_items=15U&~absent;
        assert(drive(30000)==1 && completed==1 && raw_packets && capture_warnings==1);
        for(unsigned item=0;item<4;item++)assert(item_attempts[item]==((absent&(1U<<item))?0U:2U));
        assert(gateway_log_flush()==1);
    }
    // A gateway must not request an item the batch explicitly could not capture.
    begin();flash_snapshot.valid_mask&=~4U;assert(ZbApp_PrepareScalarDelivery(9,&scalar,0));
    scalar.waiting=1;uint8_t unavailable_ack[V2_ACK_LEN];v2_ack(unavailable_ack,SCALAR_ACK_KIND,9,0,V2_MISSING,4);
    scalar_tx_receive(&scalar,unavailable_ack,sizeof(unavailable_ack),0);
    assert(!scalar.pending && scalar.waiting);
    begin();assert(sqlite3_exec(g_db,"CREATE TRIGGER reject_capture_policy BEFORE UPDATE OF sensor_mask ON mqtt_batch_state BEGIN SELECT RAISE(ABORT,'test policy save failure'); END;",NULL,NULL,NULL)==SQLITE_OK);
    assert(drive(30000)==-1 && !raw_packets && !scalar.done);
    assert(sqlite3_exec(g_db,"DROP TRIGGER reject_capture_policy;",NULL,NULL,NULL)==SQLITE_OK);
    // A different node/batch cannot fill the missing fields of this batch.
    begin();assert(record_collection_temp(node_mac,8,-12.34f)==1);
    assert(scalar_db_missing(node_mac,scalar.manifest)==15);
    const char *other="11:12:13:14:15:16:17:18";assert(insert_new_device(other,"other")==0);
    assert(record_collection_temp(other,9,-12.34f)==1);assert(scalar_db_missing(node_mac,scalar.manifest)==15);
    // Durable data survives receiver state loss, and rechecks need no value retransmission.
    assert(drive(30000)==1);assert(!scalar_db_missing(node_mac,scalar.manifest));
    for(unsigned i=0;i<V2_GATEWAY_SLOTS;i++)v2_rx_reset(&v2_gateway_slots[i].rx);
    memset(v2_gateway_slots,0,sizeof(v2_gateway_slots));memset(&v2_upload_admission,0,sizeof(v2_upload_admission));
    uint8_t manifest_copy[SCALAR_MANIFEST_LEN];memcpy(manifest_copy,scalar.manifest,sizeof(manifest_copy));
    assert(scalar_tx_start(&scalar,scalar.manifest,now_ms)); // Alias-safe reset after NEED_BATCH.
    assert(!memcmp(manifest_copy,scalar.manifest,sizeof(manifest_copy)));
    memset(item_attempts,0,sizeof(item_attempts));assert(drive(30000)==1);
    assert(!item_attempts[0] && !item_attempts[1] && !item_attempts[2] && !item_attempts[3]);
    begin();metadata_status=0;assert(!ZbApp_PrepareScalarDelivery(9,&scalar,0));
    metadata_status=-1;assert(!ZbApp_PrepareScalarDelivery(9,&scalar,0));
    begin();flash_snapshot.scalar_policy_version=2;assert(!ZbApp_PrepareScalarDelivery(9,&scalar,0));
    begin();flash_snapshot.scalar_policy_version=0;flash_snapshot.valid_mask=19;
    assert(ZbApp_PrepareScalarDelivery(9,&scalar,0) && scalar.manifest[6]==3);
    assert(drive(30000)==1 && !item_attempts[2] && !item_attempts[3]);
    // Dead transport and stale replies cannot authorize raw data or wait forever.
    begin();backpressure=1;assert(drive(30000)==-1 && !raw_packets && scalar.error);
    begin();scalar.waiting=1;uint8_t stale[V2_ACK_LEN];v2_ack(stale,SCALAR_ACK_KIND,8,0,V2_OK,0);
    scalar_tx_receive(&scalar,stale,sizeof(stale),0);assert(!scalar.done && scalar.waiting);
    v2_ack(stale,SCALAR_ACK_KIND,9,0,V2_MISSING,16);scalar_tx_receive(&scalar,stale,sizeof(stale),0);
    assert(!scalar.pending && scalar.waiting);
    // A features-only report still carries its batch ID without any scalar item.
    Custom_APP_ZigbeeSnapshot_t summary={.valid_mask=16,.vibration_features_ready=1};
    ZbApp_SendSnapshot(&summary,9);assert(summaries==1 && ZbApp.report_has_summary_confirm);
    // Rounding must recover every possible signed raw integer, especially magnetic values.
    for(int raw_value=-32768;raw_value<=32767;raw_value++) {
        int16_t restored;assert(scalar_db_integer(raw_value*0.15f,1.0/0.15,&restored) && restored==raw_value);
        assert(scalar_db_integer(raw_value/100.0f,100.0,&restored) && restored==raw_value);
    }
    gateway_log_close();close_database();puts("Scalar pre-waveform gate: repair delivery loss, skip declared capture failures, durable WARN logs, actual node/Flash/gateway/SQLite, zero values, lost ACK, old policy, retention and restart PASS");
}
