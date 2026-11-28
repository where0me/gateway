/* Real SQLite, gateway receive path and extracted node completion handlers. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
static struct tm *test_localtime_r(const time_t *n,struct tm *out){*out=*localtime(n);return out;}
#define localtime_r test_localtime_r
#endif
#define GATEWAY_DATABASE_PATH "completion.db"
#include "DB_IMPLEMENTATION"
#include "vibration_tx.h"
#include "vibration_rx.h"
#include "upload_completion.h"
#include "link_health.h"
#define ZB_WPAN_STATUS_NO_ACK 0xe9U
#define ZB_APS_STATUS_NO_ACK 0xa7U
typedef int TestMutex;
static void lock(TestMutex *m){assert(!*m);*m=1;}
static void unlock(TestMutex *m){assert(*m);*m=0;}
#define pthread_mutex_lock lock
#define pthread_mutex_unlock unlock
static TestMutex g_vibration_mutex,g_uart_mutex,g_ws_mutex;
static int g_uart_fd=1,g_has_new_ws_msg;
static char g_ws_msg_buffer[1024];
static uint32_t now_ms=40000;
static int test_clock(int id,struct timespec *out){(void)id;out->tv_sec=now_ms/1000;out->tv_nsec=now_ms%1000*1000000;return 0;}
#define clock_gettime test_clock
static const char *mac="01:02:03:04:05:06:07:08",*other="11:12:13:14:15:16:17:18";
static int lookup_mac_by_short(uint16_t address,char out[24])
{if(address!=0x1234)return 0;strcpy(out,mac);return 1;}
static void publish_collection_data_ready_event(const char *m,uint32_t b,int ready)
{(void)m;(void)b;(void)ready;}
static uint8_t ack[V2_ACK_LEN];static unsigned replies,ack_kind,acks_to_flash,tasks;
static uint32_t acknowledged_batch;
static void send_hex_frame_to_uart(int fd,uint8_t command,uint8_t len,const uint8_t *p)
{assert(fd==1 && command==V2_ACK && len==19 && v2_u16(p)==0x1234);memcpy(ack,p+2,V2_ACK_LEN);replies++;ack_kind=ack[1];}
#define UPLOAD_ADMISSION_EVENT(entry,reason,now) ((void)(entry),(void)(reason),(void)(now))
#include "vibration_gateway.h"

static V2Tx ZbVibrationUpload;
static struct {uint32_t batch;uint8_t active,confirmed,feature_only;struct {uint8_t manifest[24];} scalars;} ZbDeferred;
static LinkHealth upload_health;
static UploadLease upload_lease;
static uint32_t upload_generation;
static uint8_t ZbUploadQueueSleeping,ZbUploadQueueWake;
static void ZbApp_Log(const char *f,...){(void)f;}
static void ZbApp_TraceResult(unsigned seq,unsigned status,unsigned late){(void)seq;(void)status;(void)late;}
static void UTIL_SEQ_SetTask(unsigned task,unsigned priority){(void)task;(void)priority;tasks++;}
#define CFG_TASK_ZIGBEE_UPLOAD 1U
#define CFG_SCH_PRIO_0 0U
#define ZB_STATUS_SUCCESS 0U
typedef struct {uint32_t generation,batch,submitted_ms;uint16_t block;uint8_t command,busy,frame[82];} ZbUploadRadioSlot_t;
struct ZbApsdeDataConfT {unsigned status;};
static uint32_t HAL_GetTick(void){return now_ms;}
static void Custom_APP_ZigbeeActivityChanged(void){}
static int flash_write_fails;
static int ZigbeeVibrationStore_Acknowledge(uint32_t batch)
{if(flash_write_fails)return 0;acks_to_flash++;acknowledged_batch=batch;return 1;}
static int ZigbeeVibrationStore_ProtectBatch(uint32_t batch){assert(!batch);return 1;}
static void ZbApp_ClearDeferredUpload(void);
static int ZbApp_AcceptCompletedBatch(const uint8_t *,size_t);
static int ZbApp_ReleaseConfirmedBatch(void);
static void ZbApp_UploadConfirm(struct ZbApsdeDataConfT *,void *);

static uint8_t descriptor[V2_BATCH_LEN],manifest[24],raw[72];
static int32_t features[3][5],pending[3][5];
static void request_batch(int enhanced,unsigned raw_required)
{
    uint8_t p[2+UPLOAD_COMPLETION_BATCH_LEN];v2_w16(p,0x1234);memcpy(p+2,descriptor,V2_BATCH_LEN);
    p[2+V2_BATCH_LEN]=UPLOAD_COMPLETION_VERSION;p[3+V2_BATCH_LEN]=(uint8_t)raw_required;
    unsigned before=replies;handle_vibration_v2(V2_BATCH,p,2+(enhanced?UPLOAD_COMPLETION_BATCH_LEN:V2_BATCH_LEN));assert(replies==before+1);
}
static void request_lease(uint32_t nonce,int enhanced)
{
    uint8_t p[13];v2_w16(p,0x1234);p[2]=V2_VERSION;p[3]=UPLOAD_LEASE_KIND;
    v2_w32(p+4,v2_u32(descriptor+1));v2_w32(p+8,nonce);p[12]=UPLOAD_COMPLETION_VERSION;
    unsigned before=replies;handle_vibration_v2(V2_QUERY,p,enhanced?13:12);assert(replies==before+1);
}
static void node_context(int batch_phase,unsigned raw_required)
{
    memset(&ZbDeferred,0,sizeof(ZbDeferred));ZbDeferred.active=1;ZbDeferred.batch=v2_u32(descriptor+1);
    ZbDeferred.feature_only=!raw_required;memcpy(ZbDeferred.scalars.manifest,manifest,24);
    assert(v2_tx_start(&ZbVibrationUpload,descriptor,now_ms));
    if(!batch_phase)ZbVibrationUpload.phase=V2_TX_FINAL;
    upload_generation++;memset(&upload_health,0,sizeof(upload_health));
    upload_lease_start(&upload_lease,ZbDeferred.batch,100,now_ms);
}
int main(void)
{
    assert(init_database()==0);assert(insert_new_device(mac,"node")==0);assert(insert_new_device(other,"other")==0);
    v2_gateway_boot_known=1;v2_gateway_boot_ms=0;
    descriptor[0]=2;v2_w32(descriptor+1,9);v2_w16(descriptor+5,26667);v2_w32(descriptor+7,12);v2_w16(descriptor+11,V2_POINTS);
    for(unsigned i=0;i<sizeof(raw);i++)raw[i]=(uint8_t)(i*7U);v2_w32(descriptor+13,v2_crc(raw,sizeof(raw)));
    for(unsigned i=0;i<15;i++){features[i/5][i%5]=(int32_t)i;pending[i/5][i%5]=INT32_MIN;v2_w32(descriptor+17+4*i,INT32_MIN);}
    manifest[0]=2;v2_w32(manifest+1,9);manifest[5]=SCALAR_POLICY_VERSION;manifest[6]=manifest[7]=SCALAR_ALL;
    for(unsigned item=0;item<4;item++){uint8_t wire[10]={0};v2_w32(wire,9);scalar_w32(manifest+8+4*item,scalar_value_crc(item,wire));}

    /* Incomplete batches take the ordinary permission/repair route. */
    request_batch(1,1);assert(ack_kind==V2_KIND_BATCH && ack[8]==V2_OK);
    assert(remember_upload_manifest(mac,9,manifest)==0);
    UploadBatchReceipt_t r;assert(!get_completed_upload_batch(mac,9,&r));
    assert(save_vibration_batch(mac,9,26667,12,raw,sizeof(raw),pending)==0);
    assert(!get_completed_upload_batch(mac,9,&r)); /* raw without features */
    assert(save_vibration_batch(mac,9,26667,12,NULL,0,features)==0);
    assert(get_completed_upload_batch(mac,9,&r)==1);
    assert(!v2_gateway_complete_reply(mac,0x1234,9,123)); /* scalars absent */
    assert(record_collection_temp(mac,9,0)>=0);assert(record_collection_mag(mac,9,0,0,0)>=0);
    assert(record_collection_sound(mac,9,1,0)>=0);assert(record_collection_sound(mac,9,0,0)>=0);
    assert(record_collection_temp(mac,9,1)>=0);assert(!v2_gateway_complete_reply(mac,0x1234,9,123)); /* CRC mismatch */
    assert(record_collection_temp(mac,9,0)>=0);
    /* The actual scalar-manifest handler persists the receipt evidence. */
    assert(sqlite3_exec(g_db,"UPDATE upload_batch_receipts SET manifest=NULL WHERE batch_id=9;",0,0,0)==SQLITE_OK);
    uint8_t scalar_frame[2+SCALAR_MANIFEST_LEN];v2_w16(scalar_frame,0x1234);memcpy(scalar_frame+2,manifest,24);
    handle_scalar_manifest(scalar_frame,sizeof(scalar_frame));assert(ack_kind==SCALAR_ACK_KIND && ack[8]==V2_OK);
    assert(get_completed_upload_batch(mac,9,&r)==1 && !memcmp(r.manifest,manifest,24));

    /* Simulate lost final ACK: gateway releases and grants another node. */
    upload_release(&v2_upload_admission,mac,9,now_ms);
    assert(!upload_request(&v2_upload_admission,other,20,1,now_ms));
    node_context(0,1);uint8_t q[10];assert(upload_lease_query(&upload_lease,now_ms,q));
    uint32_t old_generation=upload_generation;
    request_lease(upload_lease.nonce,1);assert(ack_kind==UPLOAD_COMPLETION_KIND);
    uint8_t good[V2_ACK_LEN];memcpy(good,ack,sizeof(good));
    /* Drop the receipt too: repeated renewal returns the identical proof. */
    request_lease(upload_lease.nonce,1);assert(!memcmp(ack,good,sizeof(good)));
    assert(upload_owner(&v2_upload_admission,other,20,now_ms));
    assert(!upload_owner(&v2_upload_admission,mac,9,now_ms));
    /* Wrong nonce, batch, fingerprint and old unbound BATCH reply cannot delete. */
    ack[9]^=1;assert(!ZbApp_AcceptCompletedBatch(ack,sizeof(ack)));memcpy(ack,good,sizeof(ack));
    ack[2]^=1;assert(!ZbApp_AcceptCompletedBatch(ack,sizeof(ack)));memcpy(ack,good,sizeof(ack));
    ack[13]^=1;assert(!ZbApp_AcceptCompletedBatch(ack,sizeof(ack)));memcpy(ack,good,sizeof(ack));
    v2_w32(ack+9,0);assert(!ZbApp_AcceptCompletedBatch(ack,sizeof(ack)));memcpy(ack,good,sizeof(ack));
    assert(!ZbApp_AcceptCompletedBatch(ack,sizeof(ack)-1));assert(!acks_to_flash);
    assert(ZbApp_AcceptCompletedBatch(ack,sizeof(ack)) && ZbDeferred.confirmed && tasks);
    assert(!acks_to_flash && ZbVibrationUpload.phase==V2_TX_IDLE && upload_generation!=old_generation);
    ZbUploadRadioSlot_t stale={.generation=old_generation,.busy=1};struct ZbApsdeDataConfT failed={0xe9};
    ZbApp_UploadConfirm(&failed,&stale);assert(!upload_health.pending && !upload_health.failures && !stale.busy);
    flash_write_fails=1;assert(ZbApp_ReleaseConfirmedBatch()==-1 && ZbDeferred.active && !acks_to_flash);
    flash_write_fails=0;assert(ZbApp_ReleaseConfirmedBatch()==1 && acks_to_flash==1 && acknowledged_batch==9);
    assert(!ZbApp_ReleaseConfirmedBatch() && acks_to_flash==1);

    /* Metadata survives backend restart and bypasses restart guard/other owner. */
    close_database();assert(init_database()==0);memset(v2_gateway_slots,0,sizeof(v2_gateway_slots));
    memset(&v2_upload_admission,0,sizeof(v2_upload_admission));
    v2_gateway_boot_known=0;assert(!upload_request(&v2_upload_admission,other,20,1,now_ms));
    node_context(1,1);request_batch(1,1);assert(ack_kind==UPLOAD_COMPLETION_KIND && v2_u32(ack+9)==0);
    ZbUploadQueueSleeping=ZbUploadQueueWake=1;
    assert(ZbApp_AcceptCompletedBatch(ack,sizeof(ack)));assert(ZbApp_ReleaseConfirmedBatch()==1);
    assert(!ZbUploadQueueSleeping && !ZbUploadQueueWake);
    assert(upload_owner(&v2_upload_admission,other,20,now_ms));
    uint8_t changed[V2_BATCH_LEN];memcpy(changed,descriptor,sizeof(changed));changed[13]^=1;
    assert(remember_upload_batch(mac,changed,1)==-1);
    assert(remember_upload_batch(mac,descriptor,0)==-1);
    assert(!get_completed_upload_batch(other,9,&r));
    /* An old node receives ordinary old-protocol replies, never kind 8. */
    request_lease(77,0);assert(ack_kind==UPLOAD_LEASE_KIND && ack[8]==V2_NEED_BATCH);
    request_batch(0,1);assert(ack_kind==V2_KIND_BATCH && ack[8]==V2_WAIT);

    /* Real queue branch must persist/push capture-complete waiting, before
     * a descriptor exists, while preserving other nodes and previous data. */
    now_ms+=35001U; /* Finish the separate backend-restart drain guard. */
    v2_w32(descriptor+1,12);
    assert(update_device_collection_batch_state(mac,"capturing",0,12)==0);
    request_batch(0,1);assert(ack[8]==V2_WAIT && g_has_new_ws_msg);
    assert(strstr(g_ws_msg_buffer,"\"batch_id\":12") &&
           strstr(g_ws_msg_buffer,"\"status\":\"queued\"") &&
           strstr(g_ws_msg_buffer,"\"collection_state\":\"waiting\"") &&
           strstr(g_ws_msg_buffer,"\"received_packets\":0"));
    DeviceRecord_t *records=NULL;int record_count=get_all_devices(&records),found=0;
    assert(record_count==2);
    for(int i=0;i<record_count;++i) {
        if(!strcmp(records[i].mac,mac)) {
            assert(!strcmp(records[i].collection_state,"waiting") && records[i].collection_batch_id==12 &&
                   records[i].capture_batch_id==12 && records[i].capture_pending);found=1;
        } else assert(!strcmp(records[i].collection_state,"idle"));
    }
    free(records);assert(found && get_completed_upload_batch(mac,9,&r)==1);
    assert(upload_owner(&v2_upload_admission,other,20,now_ms));

    /* Feature-only policy is explicit; declared capture failures can finish. */
    v2_w32(descriptor+1,10);v2_w32(manifest+1,10);manifest[7]=1;
    uint8_t temp[6]={0};v2_w32(temp,10);scalar_w32(manifest+8,scalar_value_crc(0,temp));
    assert(remember_upload_batch(mac,descriptor,0)==0);assert(remember_upload_manifest(mac,10,manifest)==0);
    assert(save_vibration_batch(mac,10,26667,12,NULL,0,features)==0);assert(record_collection_temp(mac,10,0)>=0);
    node_context(1,0);request_batch(1,0);assert(ack_kind==UPLOAD_COMPLETION_KIND);
    assert(ZbApp_AcceptCompletedBatch(ack,sizeof(ack)));assert(ZbApp_ReleaseConfirmedBatch()==1 && acknowledged_batch==10);
    /* Declaring raw required disallows summary-only completion. */
    v2_w32(descriptor+1,11);v2_w32(manifest+1,11);v2_w32(temp,11);scalar_w32(manifest+8,scalar_value_crc(0,temp));
    assert(remember_upload_batch(mac,descriptor,1)==0);assert(remember_upload_manifest(mac,11,manifest)==0);
    assert(save_vibration_batch(mac,11,26667,12,NULL,0,features)==0);assert(!get_completed_upload_batch(mac,11,&r));
    /* Current raw bytes must still match the durable identity. */
    assert(sqlite3_exec(g_db,"UPDATE vibration_batches SET samples=zeroblob(72) WHERE batch_id=9;",0,0,0)==SQLITE_OK);
    assert(!get_completed_upload_batch(mac,9,&r));
    close_database();puts("Completed upload replay PASS: lost final ACK, active other node, SQLite restart, scalar/raw/features checks, nonce/identity rejection, task-only Flash release, stale callback, feature-only policy and legacy compatibility");
}
