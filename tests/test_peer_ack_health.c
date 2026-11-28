/* Runner appends the real firmware send/callback/failure-exit functions. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vibration_tx.h"
#include "upload_lease.h"
#include "upload_completion.h"
#include "link_health.h"
#define ZB_WPAN_STATUS_NO_ACK 0xe9U
#define ZB_APS_STATUS_NO_ACK 0xa7U
#define UNUSED(x) ((void)(x))
#define CFG_TASK_ZIGBEE_UPLOAD 1
#define CFG_SCH_PRIO_0 0
#define ZB_APP_ENDPOINT 1
#define HY_DATA_CLUSTER 0xfc11
#define ZB_APSDE_ADDRMODE_SHORT 2
#define ZB_APSDE_DATAREQ_TXOPTIONS_SECURITY 1
#define ZB_APSDE_DATAREQ_TXOPTIONS_NWKKEY 2
#define ZB_APSDE_DATAREQ_TXOPTIONS_ACK 4
#define ZB_MONITOR_ERROR 4
#define ZB_MONITOR_WAITING 1
#define ZB_MONITOR_SLEEP 0
#define ZB_CONFIG_QUERY_COLLECTION 2
enum ZbStatusCodeT {ZB_STATUS_SUCCESS=0};
struct ZbApsdeDataConfT {unsigned status;};
struct ZbApsdeDataReqT {
    struct {unsigned mode,nwkAddr,endpoint;} dst;
    unsigned srcEndpt,profileId,clusterId,asduLength,txOptions,discoverRoute;
    uint8_t *asdu;
};
typedef struct {
    uint8_t busy,frame[82],command;
    uint32_t submitted_ms,batch,generation;
    uint16_t block;
} ZbUploadRadioSlot_t;
static struct {
    void *zb,*data_server;
    uint8_t upload_timer,upload_allowed,radio_exclusive,monitor_enabled,monitor_state;
    uint8_t upload_sequence;
    uint32_t batch_id;
} ZbApp;
static struct {uint32_t batch;uint8_t feature_only;} ZbDeferred;
static uint8_t coordinator_endpoint,ZbUploadQueueSleeping,post_upload_config_due;
static uint8_t network_authorized=1U;
static uint32_t upload_generation;
static LinkHealth upload_health;
static V2Tx ZbVibrationUpload;
static UploadLease upload_lease;
static ZbUploadRadioSlot_t ZbUploadSlots[3];
static unsigned submit_status,submits,tasks,timers,controls,fail_reports;
static uint32_t HAL_GetTick(void){return 100;}
static int APP_ZIGBEE_IsConnected(void){return 1;}
static void Custom_APP_ZigbeeActivityChanged(void){}
static void ZbApp_Log(const char *fmt,...){(void)fmt;}
static void ZbApp_TracePacket(unsigned a,unsigned b,unsigned c,const uint8_t *d,size_t e)
{(void)a;(void)b;(void)c;(void)d;(void)e;}
static void ZbApp_TraceResult(unsigned a,unsigned b,unsigned c){(void)a;(void)b;(void)c;}
static void ZbApp_TraceDump(unsigned a){(void)a;}
#define UT_NODE_TX 11
static void UTIL_SEQ_SetTask(unsigned a,unsigned b){(void)a;(void)b;tasks++;}
static enum ZbStatusCodeT ZbApsdeDataReqCallback(void *zb,struct ZbApsdeDataReqT *r,
    void (*callback)(struct ZbApsdeDataConfT *,void *),void *arg)
{(void)zb;(void)callback;(void)arg;assert(r->asdu);submits++;return (enum ZbStatusCodeT)submit_status;}
static void ZbApp_ClearDeferredUpload(void){memset(&ZbDeferred,0,sizeof(ZbDeferred));}
static void ZbApp_StopUploadFastPoll(void){}
static int ZbApp_SendMonitorStateForBatch(unsigned state,uint32_t batch)
{assert(state==ZB_MONITOR_ERROR && batch==42);fail_reports++;return 0;}
static void ZbApp_StartTimer(unsigned id,unsigned delay){(void)id;assert(delay==1000);timers++;}
static void ZbApp_OpenControlWindow(unsigned reason){assert(reason==ZB_CONFIG_QUERY_COLLECTION);controls++;}
static void ZbApp_ScheduleMonitorStandby(void){controls++;}
static void ZbApp_UploadConfirm(struct ZbApsdeDataConfT *,void *);
static int ZbApp_UploadSend(void *,uint8_t,const uint8_t *,size_t);
static void actual_transfer_failed(void);
static void reset(void)
{
    memset(&upload_health,0,sizeof(upload_health));memset(&ZbVibrationUpload,0,sizeof(ZbVibrationUpload));
    memset(ZbUploadSlots,0,sizeof(ZbUploadSlots));memset(&ZbApp,0,sizeof(ZbApp));
    ZbApp.zb=ZbApp.data_server=&ZbApp;ZbDeferred.batch=42;coordinator_endpoint=8;
    ZbVibrationUpload.phase=V2_TX_STREAM;upload_generation=7;
    post_upload_config_due=0U;
    submit_status=submits=tasks=timers=controls=fail_reports=0;
}
static void confirm(unsigned status)
{
    ZbUploadSlots[0].generation=upload_generation;
    struct ZbApsdeDataConfT result={status};ZbApp_UploadConfirm(&result,&ZbUploadSlots[0]);
}
int main(void)
{
    /* Exhaustive SDK status-byte allowlist; all other failures stay neutral. */
    for(unsigned status=1;status<256;status++) {
        reset();for(unsigned i=0;i<4;i++)confirm(status);
        assert(upload_health.pending==(status==0xe9 || status==0xa7));
        assert(upload_health.failures==((status==0xe9 || status==0xa7)?4:0));
    }
    reset();confirm(0xe9);confirm(0xa7);confirm(0xe9);
    confirm(0xe1);confirm(0x70);confirm(0xf1);assert(upload_health.failures==3 && !upload_health.pending);
    confirm(0);assert(!upload_health.failures);confirm(0xe9);assert(upload_health.failures==1);
    /* Retired callbacks cannot reset or increment the current streak. */
    struct ZbApsdeDataConfT no_ack={0xe9},ok={0};ZbUploadSlots[0].generation=6;
    ZbApp_UploadConfirm(&no_ack,&ZbUploadSlots[0]);ZbApp_UploadConfirm(&ok,&ZbUploadSlots[0]);
    assert(upload_health.failures==1 && !upload_health.pending);
    /* A synchronous rejection is neutral even if it happens to return NO_ACK. */
    const uint8_t fragment[8]={0};
    for(unsigned status=1;status<256;status++) {
        reset();submit_status=status;upload_health.failures=3;
        for(unsigned i=0;i<6;i++)assert(!ZbApp_UploadSend(NULL,V2_DATA,fragment,sizeof(fragment)));
        assert(submits==6 && upload_health.failures==3 && !upload_health.pending);
    }
    reset();assert(ZbApp_UploadSend(NULL,V2_DATA,fragment,sizeof(fragment)));
    assert(!ZbApp_UploadSend(NULL,V2_DATA,fragment,sizeof(fragment)) && submits==1);
    confirm(0xe9);assert(upload_health.failures==1);
    /* Historical diagnostics, local failure, or an app timeout do not bypass
     * the filtered consecutive-ACK threshold at the real batch failure exit. */
    reset();upload_health.failures=3;ZbVibrationUpload.radio_failures=7;
    ZbVibrationUpload.timeout_count=5;actual_transfer_failed();
    assert(!upload_health.pending && upload_health.failures==3 && !timers && controls==1 && fail_reports==1 && post_upload_config_due);
    reset();for(unsigned i=0;i<4;i++)confirm(0xe9);actual_transfer_failed();
    assert(upload_health.pending && timers==1 && !controls && post_upload_config_due);
    puts("Peer ACK health PASS: all statuses, sync rejection, CCA/resource exclusion, success/late callbacks, batch failure isolation");
}
