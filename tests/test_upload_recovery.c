/* The runner appends actual firmware recovery, probe and callback functions. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vibration_tx.h"
#include "upload_lease.h"
#include "link_health.h"
#define ZB_WPAN_STATUS_NO_ACK 0xe9U
#define ZB_APS_STATUS_NO_ACK 0xa7U
#define CFG_TASK_ZIGBEE_UPLOAD 1
#define CFG_TASK_ZIGBEE_NETWORK_FORM 2
#define CFG_TASK_ZIGBEE_ANNOUNCE 3
#define CFG_TASK_ZIGBEE_REPORT 4
#define CFG_TASK_ZIGBEE_TX 5
#define CFG_TASK_ZIGBEE_RADIO_CONTROL 6
#define CFG_SCH_PRIO_0 0
#define CFG_SCH_PRIO_1 1
#define ZB_CMD_MONITOR_STATE 0x8d
#define ZB_CMD_VIBRATION_SUMMARY 0x98
#define ZB_MONITOR_WAITING 1
#define ZB_MONITOR_SLEEP 0
#define ZB_TX_MAX_IN_FLIGHT 1
#define ZB_SAVED_CHANNEL_REJOIN_ATTEMPTS 2
#define ZB_RTC_JOIN_MAX_ATTEMPTS 3
#define ZB_COMMISSIONING_JOIN_RETRY_MS 5000
#define ZB_JOIN_RETRY_MS 5000
#define CFG_ZIGBEE_NVM_ENABLED 0
#define ZB_SLEEPY_PERSIST_AUTOSAVE_ENABLED 0
enum ZbStatusCodeT {ZB_STATUS_SUCCESS=0};
enum ZclStatusCodeT {ZCL_STATUS_SUCCESS=0};
struct ZbZclCommandRspT {uint8_t aps_status,status;};
struct ZbZdoNodeDescReqT {uint16_t dstNwkAddr,nwkAddrOfInterest;};
struct ZbZdoNodeDescRspT {uint8_t status;uint16_t nwkAddr;};
static struct {
    void *zb;
    uint8_t radio_exclusive,persist_tx_hold,restore_pending,upload_timer,upload_allowed;
    uint8_t parent_online,parent_state_known,parent_sync_pending,rtc_join_active,restore_recovery_active;
    uint8_t startup_wait_active,rtc_join_attempts,rejoin_probe_pending,rejoin_probe_rtc,join_retry_timer;
    uint8_t join_blocked_until_reset,saved_channel_rejoin_attempts,commissioning_window_open;
    uint8_t tx_head,tx_tail,tx_count,tx_in_flight,radio_control_deferred;
    uint8_t report_has_summary_confirm,manual_announce_required,rejoin_via_startup;
    uint8_t reporting_active,monitor_enabled,monitor_state,offline_capture_requested,pending_snapshot_valid;
    uint32_t rejoin_channel_mask;
    enum ZbStatusCodeT join_status;
} ZbApp;
static uint8_t post_upload_config_due,network_authorized=1,ZbReceiverOnWhenIdle;
static uint8_t PendingLink_Eligible(void) { return 1; }
static void PendingLink_Stop(void) {}
static uint8_t recovery_announce_pending,ZbUploadQueueSleeping,admission_expired;
static uint8_t coordinator_endpoint,hy_zdo_inflight,hy_parent_response,recovery_config_pending;
static unsigned schedule_requests;
static void ZbApp_RequireSamplingSchedule(void){schedule_requests++;}
static uint32_t now,upload_generation,upload_nonce,command_confirm_generation;
static LinkHealth upload_health;
static UploadLease upload_lease;
static V2Tx ZbVibrationUpload;
static struct {uint32_t batch,deadline;uint8_t active,queued;} ZbDeferred;
static uint32_t protected_batch;
static int ZigbeeVibrationStore_ProtectBatch(uint32_t batch){protected_batch=batch;return 1;}
static void ZbApp_ClearDeferredUpload(void);
typedef struct {uint8_t busy,frame[82];uint32_t submitted_ms,batch,generation;uint16_t block;uint8_t command;} ZbUploadRadioSlot_t;
struct ZbApsdeDataConfT {uint8_t status;};
static unsigned probes,stopped,timers,tasks,cancelled,completed,prepare_fresh,restarts;
static uint8_t probe_submit;
static uint32_t HAL_GetTick(void){return now;}
static uint32_t APP_ElapsedMs(void){return now;}
static uint8_t APP_ZIGBEE_IsConnected(void){return ZbApp.parent_online;}
static uint8_t APP_ZIGBEE_IsTransmitting(void){return ZbApp.tx_in_flight || ZbApp.tx_count;}
static void ZbApp_StartTimer(uint8_t id,uint32_t delay){(void)id;assert(delay);timers++;}
static void HW_TS_Stop(uint8_t id){(void)id;cancelled++;}
static void ZbApp_Log(const char *fmt,...){(void)fmt;}
static void ZbApp_TraceResult(unsigned seq,unsigned status,unsigned late){(void)seq;(void)status;(void)late;}
static void ZbApp_TraceDump(unsigned reason){(void)reason;}
static void ZbApp_StopUploadFastPoll(void){stopped++;}
static void Custom_APP_ZigbeeActivityChanged(void){}
static void UTIL_SEQ_SetTask(uint32_t mask,unsigned priority){(void)priority;tasks|=mask;}
static void ZbApp_SetParentOnline(uint8_t online,enum ZbStatusCodeT status)
{(void)status;ZbApp.parent_online=online;ZbApp.parent_sync_pending=0;ZbApp.restore_recovery_active=0;}
static void ZbApp_StartFreshJoinFallback(void){assert(!"Unexpected fresh join; credentials must be retained");}
static void ZbApp_PrepareFreshJoin(void){prepare_fresh++;}
static void ZbApp_CompleteJoinFailureCycle(void);
static void ZbApp_ScheduleNextReport(void){completed++;}
static int ZbApp_RestartSavedNetwork(void) {
    restarts++;upload_health.restarted=1;upload_health.restart_at=now;
    upload_health.rounds=upload_health.backoff=upload_health.verifying=0;return 1;
}
static void ZbApp_SendPendingSnapshot(void){}
static void ZbApp_StartAutonomousReporting(void){}
static void ZbApp_VerifyParent(void);
static void ZbApp_ResetTxQueue(void);
static void ZbApp_StartLinkRecovery(void);
static uint8_t ZbApp_ServiceUploadRecovery(void);
static void ZbApp_UploadConfirm(struct ZbApsdeDataConfT *,void *);
static void ZbApp_CommandConfirm(struct ZbZclCommandRspT *,void *);
static void ZbApp_ParentDescriptor(struct ZbZdoNodeDescRspT *,void *);
static enum ZclStatusCodeT ZbApp_SendMonitorState(uint8_t state)
{(void)state;probes++;return (enum ZclStatusCodeT)probe_submit;}
static void *ZbNwkFastPollRequest(void *zb,unsigned period,unsigned duration)
{(void)zb;(void)period;(void)duration;return &ZbApp;}
static enum ZbStatusCodeT ZbZdoNodeDescReq(void *zb,struct ZbZdoNodeDescReqT *r,
    void (*callback)(struct ZbZdoNodeDescRspT *,void *),void *arg)
{(void)zb;(void)callback;(void)arg;assert(!r->dstNwkAddr && !r->nwkAddrOfInterest);probes++;return (enum ZbStatusCodeT)probe_submit;}
static void reset(void)
{
    memset(&ZbApp,0,sizeof(ZbApp));memset(&upload_health,0,sizeof(upload_health));
    memset(&ZbDeferred,0,sizeof(ZbDeferred));memset(&ZbVibrationUpload,0,sizeof(ZbVibrationUpload));
    ZbApp.zb=&ZbApp;ZbApp.join_retry_timer=3;ZbApp.parent_online=1;
    recovery_announce_pending=ZbUploadQueueSleeping=admission_expired=0;
    coordinator_endpoint=8;hy_zdo_inflight=hy_parent_response=0;
    now=command_confirm_generation=0;probes=stopped=timers=tasks=cancelled=completed=prepare_fresh=restarts=0;
    probe_submit=0;
}
static void *probe_arg(uint32_t generation)
{return (void *)(uintptr_t)((generation<<8)|ZB_CMD_MONITOR_STATE);}
static void third_candidate(uint32_t at)
{
    now=at;ZbApp.rtc_join_active=1;ZbApp.rtc_join_attempts=3;
    ZbApp.rejoin_probe_pending=ZbApp.rejoin_probe_rtc=1;ZbApp.parent_sync_pending=0;
    ZbApp_VerifyParent();assert(ZbApp.parent_sync_pending);
}
int main(void)
{
    reset();ZbDeferred.batch=42;protected_batch=42;upload_generation=5;
    ZbVibrationUpload.phase=V2_TX_STREAM;
    ZbUploadRadioSlot_t slot={.busy=1,.batch=42,.generation=5,.block=11,.command=V2_DATA};
    struct ZbApsdeDataConfT fail={0xa7},ok={0};
    for(unsigned i=0;i<4;i++)ZbApp_UploadConfirm(&fail,&slot);
    assert(upload_health.pending && tasks && !slot.busy);
    assert(ZbApp_ServiceUploadRecovery() && probes==1 && !ZbApp.parent_online && stopped==1 && post_upload_config_due);
    assert(!ZbDeferred.batch && !protected_batch && ZbVibrationUpload.phase==V2_TX_IDLE && !ZbApp.upload_allowed);
    unsigned failures=upload_health.failures;
    ZbApp_UploadConfirm(&ok,&slot);assert(upload_health.failures==failures);
    now=10001;assert(ZbApp_ServiceUploadRecovery() && upload_health.backoff && !ZbApp.rtc_join_active);
    now=70001;assert(ZbApp_ServiceUploadRecovery() && ZbApp.rtc_join_active);
    assert(tasks&(1U<<CFG_TASK_ZIGBEE_NETWORK_FORM));assert(!prepare_fresh);
    ZbApp.parent_online=1;ZbApp.rtc_join_active=0;recovery_announce_pending=1;
    assert(ZbApp_ServiceUploadRecovery());
    recovery_announce_pending=0;assert(ZbApp_ServiceUploadRecovery());
    assert(!upload_health.pending && ZbApp.upload_allowed && timers);

    /* Replay 9D: third probe at 544.386s must survive old deadline 547.391s. */
    reset();ZbApp.parent_online=0;upload_health.pending=upload_health.verifying=1;
    upload_health.deadline=547391;third_candidate(544386);
    assert(upload_health.deadline==554386);
    uint32_t generation=command_confirm_generation;
    now=547391;assert(ZbApp_ServiceUploadRecovery());
    assert(command_confirm_generation==generation && ZbApp.rejoin_probe_pending && !cancelled);
    struct ZbZclCommandRspT failed_probe={.aps_status=0xe9};
    now=550386;ZbApp_CommandConfirm(&failed_probe,probe_arg(generation));
    assert(!ZbApp.rejoin_probe_pending && !ZbApp.parent_sync_pending && !ZbApp.rtc_join_active && upload_health.rounds==1);
    assert(upload_health.deadline==610386 && upload_health.backoff);
    now=565387;generation=command_confirm_generation;
    assert(ZbApp_ServiceUploadRecovery() && command_confirm_generation==generation && !ZbApp.rtc_join_active);
    now=610387;assert(ZbApp_ServiceUploadRecovery());
    assert(ZbApp.rtc_join_active && !ZbApp.rtc_join_attempts && !prepare_fresh);

    /* Lost callbacks must not leave an immortal pending candidate/RTC cycle. */
    for(unsigned i=0;i<3;i++) {
        upload_health.rounds=0;
        third_candidate(now+1000);generation=command_confirm_generation;
        hy_zdo_inflight=hy_parent_response=1;
        now=upload_health.deadline+1;assert(ZbApp_ServiceUploadRecovery());
        assert(command_confirm_generation!=generation && !ZbApp.rejoin_probe_pending && !ZbApp.rejoin_probe_rtc);
        assert(!ZbApp.parent_sync_pending && !hy_zdo_inflight && !hy_parent_response);
        assert(!ZbApp.rtc_join_active && upload_health.backoff && !prepare_fresh);
        ZbApp.parent_sync_pending=ZbApp.rejoin_probe_pending=1;
        ZbApp_CommandConfirm(&failed_probe,probe_arg(generation));
        struct ZbZclCommandRspT good_probe={0};
        ZbApp_CommandConfirm(&good_probe,probe_arg(generation));
        assert(!ZbApp.parent_online && ZbApp.parent_sync_pending && ZbApp.rejoin_probe_pending);
        now=upload_health.deadline+1;ZbApp_ServiceUploadRecovery();
        assert(ZbApp.rtc_join_active && !upload_health.backoff);
    }

    /* A synchronous startup cannot be retired by nested sequencer work. */
    uint32_t deadline=upload_health.deadline;
    ZbApp.startup_wait_active=1;now=deadline+50000;generation=command_confirm_generation;
    assert(ZbApp_ServiceUploadRecovery() && command_confirm_generation==generation);
    ZbApp.startup_wait_active=0;
    third_candidate(now);assert(upload_health.deadline==now+10000);
    struct ZbZclCommandRspT good_probe={0};
    ZbApp_CommandConfirm(&good_probe,probe_arg(command_confirm_generation));
    assert(ZbApp.parent_online && !ZbApp.rtc_join_active && !ZbApp.rejoin_probe_pending);
    recovery_announce_pending=1;assert(ZbApp_ServiceUploadRecovery());
    recovery_announce_pending=0;assert(ZbApp_ServiceUploadRecovery() && ZbApp.upload_allowed);

    /* ZDO gets the same budget; retired ZDO cannot clear a newer probe. */
    reset();ZbApp.parent_online=0;coordinator_endpoint=0;
    upload_health.pending=upload_health.verifying=1;upload_health.deadline=5;now=4000;
    ZbApp_VerifyParent();assert(hy_zdo_inflight && upload_health.deadline==14000);
    generation=command_confirm_generation;now=14001;assert(ZbApp_ServiceUploadRecovery());
    ZbApp.parent_sync_pending=hy_zdo_inflight=1;
    struct ZbZdoNodeDescRspT descriptor={0};
    ZbApp_ParentDescriptor(&descriptor,(void *)(uintptr_t)generation);
    assert(hy_zdo_inflight && !hy_parent_response);
    ZbApp_ParentDescriptor(&descriptor,(void *)(uintptr_t)command_confirm_generation);
    assert(!hy_zdo_inflight && hy_parent_response==1);

    /* Failed submission does not pretend that a new probe has started. */
    reset();upload_health.pending=upload_health.verifying=1;
    upload_health.deadline=99;probe_submit=1;now=50;ZbApp_VerifyParent();
    assert(!ZbApp.parent_sync_pending && upload_health.deadline==99);
    coordinator_endpoint=0;ZbApp_VerifyParent();assert(!hy_zdo_inflight && upload_health.deadline==99);
    reset();upload_health.pending=upload_health.verifying=1;ZbApp.parent_online=0;
    now=UINT32_MAX-4999;third_candidate(now);assert(upload_health.deadline==5000);
    now=4999;generation=command_confirm_generation;assert(ZbApp_ServiceUploadRecovery());
    assert(command_confirm_generation==generation);
    now=5001;assert(ZbApp_ServiceUploadRecovery() && command_confirm_generation!=generation);
    // Completed rounds get a real 60s backoff, then escalate after two rounds.
    reset();ZbApp.parent_online=0;upload_health.pending=upload_health.verifying=1;
    ZbApp_CompleteJoinFailureCycle();assert(upload_health.deadline==60000 && upload_health.backoff);
    now=15001;assert(ZbApp_ServiceUploadRecovery() && !tasks && !restarts);
    ZbApp_StartLinkRecovery();ZbApp_VerifyParent();assert(!tasks && !probes);
    now=60000;ZbApp_ServiceUploadRecovery();assert(ZbApp.rtc_join_active);
    ZbApp_CompleteJoinFailureCycle();assert(upload_health.rounds==2);
    now=119999;ZbApp_ServiceUploadRecovery();assert(!restarts);
    now=120000;ZbApp_ServiceUploadRecovery();assert(restarts==1 && !upload_health.backoff);
    upload_health.verifying=1;ZbApp_CompleteJoinFailureCycle();ZbApp_CompleteJoinFailureCycle();
    now=180000;ZbApp_ServiceUploadRecovery();assert(restarts==1); // Restart cooldown, normal rejoin allowed.
    reset();upload_health.pending=upload_health.verifying=upload_health.backoff=1;
    upload_health.deadline=60000;ZbApp.parent_online=1;
    assert(ZbApp_ServiceUploadRecovery() && !upload_health.pending && ZbApp.upload_allowed);
    puts("Actual recovery PASS: 60s backoff ownership, bounded restart escalation/cooldown, 9D probe timing, lost/late callbacks, startup isolation, RTC wrap and oldest-batch reapply");
}
