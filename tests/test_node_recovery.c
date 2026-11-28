/* run-node-recovery-tests.ps1 appends the real firmware function bodies. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "monitor_standby.h"
#include "link_health.h"
static LinkHealth upload_health;
static uint32_t APP_ElapsedMs(void){return 0;}
static MonitorStandby monitor_standby;
/* Cadence planning itself is covered by test_reconnect_sampling.c. */
static void ZbApp_RequireSamplingSchedule(void){}
#define UNUSED(x) ((void)(x))
#define ZB_SLEEPY_PERSIST_AUTOSAVE_ENABLED 0
#define CFG_TASK_ZIGBEE_ANNOUNCE 1
#define CFG_TASK_ZIGBEE_REPORT 2
#define CFG_SCH_PRIO_0 0
#define CFG_SCH_PRIO_1 1
#define ZB_CONFIG_QUERY_JOIN 1
#define ZB_CONFIG_QUERY_COLLECTION 2
#define ZB_CONFIG_QUERY_IDLE 3
#define ZB_CONFIG_QUERY_RETRY_MS 2000
#define ZB_CMD_CONFIG_QUERY 0x99
#define ZB_MONITOR_SLEEP 0
#define ZB_MONITOR_WAITING 1
#define ZB_NWK_SYNC_DEFAULT_DELAY_MS 250
#define ZB_COMMISSIONING_FAST_POLL_MS 3000
#define ZB_JOIN_RETRY_MS 5000
#define MCP_ASSOC_CAP_ALLOC_ADDR 0x80
enum ZbStatusCodeT {ZB_STATUS_SUCCESS=0, ZB_STATUS_FAILURE=1};
enum ZclStatusCodeT {ZCL_STATUS_SUCCESS=0};
enum {ZB_NWK_NIB_ID_CapabilityInformation,ZB_NWK_NIB_ID_PanId,
      ZB_NWK_NIB_ID_ExtendedPanId,ZB_NWK_NIB_ID_ManagerAddr};
struct nwk_fastpoll_entry_t {int unused;};
struct ZbZdoDeviceAnnceT {uint16_t nwkAddr;uint64_t extAddr;uint8_t capability;};
struct ZbZdoNodeDescReqT {uint16_t dstNwkAddr,nwkAddrOfInterest;};
struct ZbZdoNodeDescRspT {int unused;};
static struct {
    void *zb;
    uint8_t radio_exclusive,restore_pending,restore_recovery_active,join_blocked_until_reset;
    enum ZbStatusCodeT join_status;
    uint8_t parent_sync_pending,parent_state_known,parent_online,commissioning_window_open;
    uint8_t saved_channel_rejoin_attempts,manual_announce_required,rejoin_identity_valid;
    uint16_t rejoin_pan_id,rejoin_short_address,rejoin_manager_address;
    uint64_t rejoin_ext_pan_id;
    uint8_t config_query_pending,config_query_reason,config_query_attempts;
    uint8_t channel_migration_wait,channel_migration_target,report_timer,join_retry_timer;
    uint8_t reporting_active,monitor_enabled,monitor_state,collection_requested,config_valid;
} ZbApp;
static uint8_t recovery_config_pending,recovery_announce_pending,recovery_token_valid;
static uint8_t admission_phase,admission_expired,admission_timer_id,network_authorized;
static uint8_t coordinator_endpoint,hy_zdo_inflight;
static uint32_t command_confirm_generation;
static uint32_t tasks;
static unsigned announcements,config_queries,verifications,timers;
static enum ZbStatusCodeT submission;
static char events[32];static unsigned event_count;
static void record(char event){assert(event_count+1<sizeof(events));events[event_count++]=event;}
static void ZbApp_AnnounceTask(void);
static void ZbApp_VerifyParent(void);
static void ZbApp_SetParentOnline(uint8_t online,enum ZbStatusCodeT status);
static void ZbApp_OpenControlWindow(uint8_t reason);
static void ZbApp_StartAutonomousReporting(void);
static void ZbApp_Log(const char *fmt,...){(void)fmt;}
static void ZbApp_SetMaximumTxPower(void){}
static void HW_TS_Stop(uint8_t id){(void)id;}
static void UTIL_SEQ_SetTask(uint32_t mask,int priority){(void)priority;tasks|=mask;}
static void ZbApp_StartTimer(uint8_t id,uint32_t ms){(void)id;assert(ms>0);timers++;}
static void ZbApp_StartAdmissionFastPoll(void){}
static void ZbApp_StartConfigTransaction(void){record('W');}
static void ZbApp_ScheduleNextReport(void){}
static void ZbApp_ScheduleMonitorStandby(void){}
static uint8_t APP_ZIGBEE_IsConnected(void){return ZbApp.parent_online;}
static uint16_t ZbShortAddress(void *zb){(void)zb;return 0x941d;}
static uint64_t ZbExtendedAddress(void *zb){(void)zb;return UINT64_C(0x0080e12500ef2c9d);}
static enum ZbStatusCodeT ZbNwkGet(void *zb,int attr,void *out,size_t size)
{
    (void)zb;(void)size;
    switch(attr){
      case ZB_NWK_NIB_ID_CapabilityInformation:*(uint8_t *)out=0x80;break;
      case ZB_NWK_NIB_ID_PanId:*(uint16_t *)out=0xfe31;break;
      case ZB_NWK_NIB_ID_ExtendedPanId:*(uint64_t *)out=UINT64_C(0x00124b002a025453);break;
      case ZB_NWK_NIB_ID_ManagerAddr:*(uint16_t *)out=0;break;
      default:assert(0);
    }
    return ZB_STATUS_SUCCESS;
}
static struct nwk_fastpoll_entry_t *ZbNwkFastPollRequest(void *zb,unsigned interval,unsigned duration)
{static struct nwk_fastpoll_entry_t handle;(void)zb;(void)interval;(void)duration;return &handle;}
static void ZbZdoDeviceAnnce(void *zb,struct ZbZdoDeviceAnnceT *a)
{
    (void)zb;assert(!ZbApp.restore_recovery_active && !ZbApp.radio_exclusive);
    assert(ZbApp.parent_online && a->nwkAddr==0x941d && a->extAddr==ZbExtendedAddress(zb));
    announcements++;record('A');
}
static void ZbApp_ParentDescriptor(struct ZbZdoNodeDescRspT *rsp,void *arg){(void)rsp;(void)arg;}
static enum ZbStatusCodeT ZbZdoNodeDescReq(void *zb,struct ZbZdoNodeDescReqT *r,
    void (*cb)(struct ZbZdoNodeDescRspT *,void *),void *arg)
{
    (void)zb;(void)cb;(void)arg;assert(r->dstNwkAddr==0 && r->nwkAddrOfInterest==0);
    verifications++;record('V');return submission;
}
static enum ZclStatusCodeT ZbApp_SendMonitorState(uint8_t state)
{(void)state;verifications++;record('P');return ZCL_STATUS_SUCCESS;}
static void begin_restore(void)
{
    memset(&ZbApp,0,sizeof(ZbApp));memset(events,0,sizeof(events));event_count=0;
    announcements=config_queries=verifications=timers=tasks=0;
    recovery_config_pending=recovery_token_valid=hy_zdo_inflight=coordinator_endpoint=0;
    recovery_announce_pending=1;admission_phase=1;admission_expired=0;network_authorized=1;
    ZbApp.zb=&ZbApp;ZbApp.join_status=ZB_STATUS_SUCCESS;
    ZbApp.restore_recovery_active=ZbApp.manual_announce_required=1;
    submission=ZB_STATUS_SUCCESS;
}
static void confirmed_parent(void)
{
    hy_zdo_inflight=0; /* Sequencer processes a current successful ZDO response. */
    ZbApp_SetParentOnline(1,ZB_STATUS_SUCCESS);
    assert(!ZbApp.restore_recovery_active && recovery_announce_pending && recovery_config_pending);
    assert(tasks&(1U<<CFG_TASK_ZIGBEE_ANNOUNCE));
}
int main(void)
{
    begin_restore();ZbApp_StartAutonomousReporting();ZbApp_OpenControlWindow(ZB_CONFIG_QUERY_JOIN);
    assert(!config_queries && !timers && recovery_config_pending);
    ZbApp_AnnounceTask();ZbApp_AnnounceTask();
    assert(verifications==1 && !announcements && ZbApp.manual_announce_required);
    confirmed_parent();coordinator_endpoint=8;ZbApp_OpenControlWindow(ZB_CONFIG_QUERY_JOIN);
    assert(!config_queries); /* Even early HELLO cannot overtake announcement. */
    ZbApp.radio_exclusive=1;ZbApp_AnnounceTask();assert(!announcements && recovery_announce_pending);
    ZbApp.radio_exclusive=0;ZbApp_AnnounceTask();
    assert(announcements==1 && !recovery_announce_pending && !ZbApp.manual_announce_required);
    ZbApp_SetParentOnline(1,ZB_STATUS_SUCCESS);ZbApp_AnnounceTask();assert(announcements==1);
    ZbApp_OpenControlWindow(ZB_CONFIG_QUERY_JOIN);assert(!config_queries && !strcmp(events,"VAW"));
    assert(network_authorized && ZbApp.rejoin_pan_id==0xfe31);

    begin_restore();coordinator_endpoint=8;ZbApp_AnnounceTask();
    assert(verifications==1 && !strcmp(events,"V")); /* Restore still uses ZDO. */
    confirmed_parent();ZbApp_AnnounceTask();assert(!config_queries);
    ZbApp_OpenControlWindow(ZB_CONFIG_QUERY_JOIN);assert(!strcmp(events,"VAW"));

    begin_restore();submission=ZB_STATUS_FAILURE;ZbApp_AnnounceTask();
    assert(!announcements && !config_queries && !hy_zdo_inflight && timers==1);
    assert(ZbApp.restore_recovery_active && recovery_announce_pending && network_authorized);
    ZbApp_SetParentOnline(0,ZB_STATUS_FAILURE);submission=ZB_STATUS_SUCCESS;ZbApp_AnnounceTask();
    confirmed_parent();ZbApp_AnnounceTask();assert(announcements==1 && !strcmp(events,"VVA"));

    begin_restore();ZbApp.restore_pending=1;ZbApp_AnnounceTask();ZbApp_VerifyParent();
    ZbApp_OpenControlWindow(ZB_CONFIG_QUERY_JOIN);assert(!verifications && !announcements && !config_queries);
    ZbApp.restore_pending=0;ZbApp_AnnounceTask();confirmed_parent();ZbApp_AnnounceTask();
    assert(!config_queries && !recovery_announce_pending);
    assert(tasks&(1U<<CFG_TASK_ZIGBEE_REPORT)); /* Generic coordinator can resume temperature work. */
    ZbApp_AnnounceTask();assert(announcements==1); /* No private endpoint: wait for HELLO. */

    begin_restore();ZbApp.restore_recovery_active=recovery_announce_pending=0;ZbApp.parent_online=1;
    ZbApp_AnnounceTask();ZbApp_AnnounceTask();assert(announcements==1 && !verifications);
    puts("Actual dual-mode node recovery: verify -> one announcement -> RX window, early HELLO, repeated tasks, BLE pause, submission failure/retry and fresh-join isolation PASS");
    return 0;
}
