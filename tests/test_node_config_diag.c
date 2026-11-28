/* The runner appends actual functions from the dual BLE/Zigbee node. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sensor_clusters.h"
#include "monitor_standby.h"
#include "control_sync.h"
#define UNUSED(x) ((void)(x))
#define ZB_TX_QUEUE_DEPTH 16U
#define ZB_TX_MAX_PAYLOAD 70U
#define ZB_TX_MAX_IN_FLIGHT 3U
#define ZB_CMD_CONFIG_QUERY 0x99U
#define ZB_CMD_VIBRATION_SUMMARY 0x8CU
#define ZB_CMD_MONITOR_STATE 0x8DU
#define ZB_MONITOR_COMPLETE 4U
#define ZB_MONITOR_SLEEP 0U
#define ZB_MONITOR_WAITING 1U
#define ZCL_BASIC_POWER_DC 4U
#define ZB_CONFIG_FLAG_MONITOR_PRESENT 4U
#define ZB_CONFIG_FLAG_MONITOR_ENABLED 2U
#define CFG_LPM_MONITOR_STANDBY 2U
#define ZB_NWK_NIB_ID_FastPollPeriod 0x0403U
#define ZB_CONFIG_QUERY_JOIN 1U
#define ZB_CONFIG_QUERY_COLLECTION 2U
#define ZB_CONFIG_QUERY_IDLE 3U
#define ZB_CONFIG_QUERY_RETRY_MS 2000U
#define ZB_CONFIG_QUERY_MAX_ATTEMPTS 3U
#define ZB_JOIN_RETRY_MS 5000U
#define ZB_COMMISSIONING_JOIN_RETRY_MS 1000U
#define ZB_RTC_JOIN_MAX_ATTEMPTS 3U
#define ZB_ZCL_UNUSED 0U
#define ZB_NWK_SYNC_DEFAULT_DELAY_MS 50U
#define ZB_APSDE_ADDRMODE_SHORT 2U
#define ZCL_NO_DEFAULT_RESPONSE_TRUE 1U
#define CFG_TASK_ZIGBEE_TX 1U
#define CFG_TASK_ZIGBEE_REPORT 2U
#define CFG_TASK_ZIGBEE_NETWORK_FORM 3U
#define CFG_TASK_ZIGBEE_RADIO_CONTROL 4U
#define CFG_SCH_PRIO_0 0U
#define CFG_SCH_PRIO_1 1U
#define CFG_LPM_APP_ZIGBEE 1U
#define UTIL_LPM_DISABLE 0U
#define UTIL_LPM_ENABLE 1U
enum ZbStatusCodeT { ZB_STATUS_SUCCESS = 0, ZB_STATUS_FAILURE = 1 };
enum ZclStatusCodeT { ZCL_STATUS_SUCCESS = 0, ZCL_STATUS_FAILURE = 1, ZCL_STATUS_INSUFFICIENT_SPACE = 0x89 };
struct ZbZclCommandRspT { int unused; };
struct ZbZclClusterCommandReqT {
    struct { unsigned mode, nwkAddr, endpoint; } dst;
    unsigned cmdId, noDefaultResp, length;
    const uint8_t *payload;
};
struct nwk_fastpoll_entry_t { int unused; };
typedef struct { uint8_t command_id, length, await_confirm, payload[ZB_TX_MAX_PAYLOAD]; } ZbTxFrame_t;
static struct {
    void *zb, *temperature_server, *control_server, *data_server;
    enum ZbStatusCodeT join_status;
    uint8_t tx_head, tx_tail, tx_count, tx_in_flight, report_has_summary_confirm;
    uint8_t parent_online, parent_sync_pending, rejoin_probe_pending, rejoin_probe_rtc;
    uint8_t commissioning_window_open, rtc_join_active, rtc_join_attempts;
    uint8_t radio_exclusive, radio_paused, radio_pause_pending, radio_control_deferred;
    uint8_t persist_tx_hold, restore_pending, restore_recovery_active;
    uint8_t config_query_pending, config_query_reason, config_query_attempts;
    uint8_t channel_migration_wait, channel_migration_target, monitor_enabled, monitor_state;
    uint8_t report_timer, join_retry_timer, config_valid;
} ZbApp;
static MonitorStandby monitor_standby;
static uint32_t applied_config_hash;
static uint8_t sampling_due,sampling_timer;
static uint8_t ZbUploadQueueSleeping;
static unsigned poll_period,standby_blocked;
static struct {unsigned active;} sampling_cadence;
static ZbTxFrame_t ZbTxQueue[ZB_TX_QUEUE_DEPTH];
static uint8_t coordinator_endpoint = 8, recovery_announce_pending, recovery_config_pending;
static uint8_t network_authorized = 1;
static uint32_t command_confirm_generation = 1, now;
static struct nwk_fastpoll_entry_t *ZbConfigFastPoll;
static unsigned submissions, tasks, timer_calls, fastpoll_calls, releases, standby_calls, next_calls, reset_calls;
static unsigned last_timer_delay, last_fastpoll_duration, diagnostic_lines;
static uint8_t submitted_payload[70];
static enum ZclStatusCodeT submit_status;
static char logs[32768];
static void ZbApp_ConfigDiag(const char *stage, uint8_t reason, unsigned int result);
static void ZbApp_ConfigDiagQueued(const char *stage, unsigned int result);
static enum ZclStatusCodeT ZbApp_SendCommand(uint8_t cmd, const uint8_t *payload, uint16_t length);
static void ZbApp_TxTask(void);
static void ZbApp_OpenControlWindow(uint8_t reason);
static uint8_t ZbApp_ServiceControlWindow(void);
static uint8_t ZbApp_ApplyMonitorSwitch(uint8_t flags);
static void ZbApp_SendLinkStatus(uint8_t applied);
static void ZbApp_ReportTimerCallback(void);
static void ZbApp_StartConfigTransaction(void);
static void ZbApp_StopConfigTransaction(void);
static void ZbApp_ScheduleMonitorStandbyImpl(void);
static uint32_t APP_ElapsedMs(void) { return now; }
static void ZbApp_Log(const char *format, ...)
{
    char line[224]; va_list args; va_start(args, format);
    int length = vsnprintf(line, sizeof(line), format, args); va_end(args);
    assert(length > 0 && length < 204); // Firmware also adds a timestamp prefix.
    if (strstr(line, "ZIGBEE-CFG:")) diagnostic_lines++;
    assert(strlen(logs) + (size_t)length < sizeof(logs)); strcat(logs, line);
}
static void UTIL_SEQ_SetTask(uint32_t mask, unsigned priority) { (void)mask; (void)priority; tasks++; }
static void Custom_APP_ZigbeeActivityChanged(void) {}
static void ZbApp_CommandConfirm(struct ZbZclCommandRspT *response, void *arg) { (void)response; (void)arg; }
static enum ZclStatusCodeT ZbZclClusterCommandReq(void *cluster, struct ZbZclClusterCommandReqT *request,
    void (*callback)(struct ZbZclCommandRspT *, void *), void *arg)
{
    assert(request->dst.mode == ZB_APSDE_ADDRMODE_SHORT && request->dst.nwkAddr == 0 && request->dst.endpoint == 8);
    assert(request->noDefaultResp == ZCL_NO_DEFAULT_RESPONSE_TRUE);
    assert(request->length <= sizeof(submitted_payload));
    if (request->cmdId == ZB_CMD_CONFIG_QUERY) {
        assert(cluster == ZbApp.control_server && callback == NULL); // No added APS/ZCL callback or in-flight slot.
        assert((uint8_t)(uintptr_t)arg == ZB_CMD_CONFIG_QUERY);
    }
    memcpy(submitted_payload, request->payload, request->length); submissions++;
    return submit_status;
}
static void ZbApp_StartTimer(uint8_t timer, uint32_t delay) { (void)timer; timer_calls++; last_timer_delay = delay; }
static void ZbApp_CompleteJoinFailureCycle(void) { assert(0); }
static void ZbApp_ResetTxQueue(void) { reset_calls++; ZbApp.tx_head = ZbApp.tx_tail = ZbApp.tx_count = 0; }
static uint8_t APP_ZIGBEE_IsConnected(void) { return ZbApp.parent_online; }
static void ZbApp_ScheduleMonitorStandby(void) { standby_calls++; ZbApp_ScheduleMonitorStandbyImpl(); }
static void ZbApp_ScheduleNextReport(void) { next_calls++; }
static void UTIL_LPM_SetStopMode(unsigned mask, unsigned mode)
{if(mask==(1U<<CFG_LPM_MONITOR_STANDBY))standby_blocked=mode==UTIL_LPM_DISABLE;}
static void HW_TS_Stop(uint8_t id){(void)id;}
static int ZbNwkSet(void *zb,unsigned attr,const void *p,size_t n)
{assert(zb && attr==ZB_NWK_NIB_ID_FastPollPeriod && n==2);poll_period=*(const uint16_t *)p;assert(poll_period==250 || poll_period==1000);return 0;}
static struct nwk_fastpoll_entry_t *ZbNwkFastPollRequest(void *zb, unsigned delay, unsigned duration)
{
    static struct nwk_fastpoll_entry_t handle;
    assert(zb && delay == ZB_NWK_SYNC_DEFAULT_DELAY_MS); fastpoll_calls++; last_fastpoll_duration = duration;
    return &handle;
}
static int ZbNwkFastPollRelease(void *zb, struct nwk_fastpoll_entry_t *handle)
{ assert(zb && handle); releases++; return 1; }
static void reset(void)
{
    memset(&ZbApp, 0, sizeof(ZbApp)); memset(ZbTxQueue, 0, sizeof(ZbTxQueue));
    ZbApp.zb = ZbApp.temperature_server = ZbApp.control_server = ZbApp.data_server = &ZbApp;
    ZbApp.parent_online = 1; ZbApp.join_status = ZB_STATUS_SUCCESS; coordinator_endpoint = 8;
    recovery_announce_pending = recovery_config_pending = 0; ZbConfigFastPoll = NULL;
    submissions = tasks = timer_calls = fastpoll_calls = releases = standby_calls = next_calls = reset_calls = 0;
    diagnostic_lines = 0; logs[0] = 0; now += 10000U; submit_status = ZCL_STATUS_SUCCESS;
    monitor_standby=(MonitorStandby){.random=123};applied_config_hash=0;
    ZbUploadQueueSleeping=standby_blocked=poll_period=0;
}
static void has_stage(const char *stage)
{
#if (ZB_CONFIG_TX_DIAG_ENABLED != 0U)
    assert(strstr(logs, stage));
#else
    (void)stage; assert(!diagnostic_lines);
#endif
}
int main(void)
{
    /* The actual receive-window path emits one small status, never 0x99. */
    reset(); assert(APP_ElapsedMs()>0); ZbApp_OpenControlWindow(ZB_CONFIG_QUERY_IDLE);
    assert(!ZbApp.tx_count && ZbApp.config_query_pending && !ZbApp.config_query_attempts);
    assert(last_timer_delay>=250 && last_timer_delay<=2000 && fastpoll_calls==1);
    ZbApp_ReportTimerCallback(); assert(ZbApp.config_query_attempts==1 && !submissions);
    assert(ZbApp_ServiceControlWindow() && ZbApp.tx_count==1 && last_timer_delay==8000);
    assert(ZbTxQueue[0].command_id==CONTROL_STATUS_CMD && ZbTxQueue[0].length==8);
    ZbApp_TxTask(); assert(submissions==1 && !ZbApp.tx_count && !ZbApp.tx_in_flight);
    assert(!ZbApp_ServiceControlWindow()); ZbApp_ReportTimerCallback();
    assert(!ZbApp.config_query_pending && releases==1 && standby_calls==1 && submissions==1);
    /* Missing initial push leaves acquisition disabled and eventually sleeps. */
    reset(); ZbApp_OpenControlWindow(1); ZbApp_ReportTimerCallback();
    ZbApp_ServiceControlWindow(); ZbApp_TxTask(); ZbApp_ReportTimerCallback();
    assert(!ZbApp.monitor_enabled && !ZbApp.config_valid && standby_calls==1 && releases==1);
    /* Recovery/HELLO ordering and unavailable parent never transmit early. */
    reset(); ZbApp.restore_pending=1; ZbApp_OpenControlWindow(1);
    assert(recovery_config_pending && !tasks && !timer_calls && !submissions);
    reset(); coordinator_endpoint=0; ZbApp_OpenControlWindow(3);
    assert(last_timer_delay==1000 && !ZbApp.tx_count);
    reset(); ZbApp.parent_online=0; ZbApp_OpenControlWindow(3);
    assert(standby_calls==1 && !ZbApp.tx_count);
    /* Confirmation carries exactly the adopted fingerprint and actual state. */
    reset(); ZbApp.config_valid=1; ZbApp.monitor_enabled=1;
    applied_config_hash=UINT32_C(0x12345678);ZbApp_SendLinkStatus(1);
    assert(ZbTxQueue[0].command_id==CONTROL_STATUS_CMD);
    assert(control_status_valid(ZbTxQueue[0].payload,8));
    assert(ZbTxQueue[0].payload[1]==7 && control_read_hash(ZbTxQueue[0].payload+2)==applied_config_hash);
    /* Duplicate ON configurations must preserve an active capture/upload. */
    reset(); assert(!ZbApp_ApplyMonitorSwitch(6) && ZbApp.monitor_enabled);
    assert(ZbApp.monitor_state==ZB_MONITOR_WAITING);
    ZbApp.monitor_state=2;sampling_cadence.active=1;sampling_due=1;
    assert(ZbApp_ApplyMonitorSwitch(6) && ZbApp.monitor_state==2 && sampling_cadence.active);
    assert(ZbApp_ApplyMonitorSwitch(4) && !ZbApp.monitor_enabled && !sampling_cadence.active && !sampling_due);
    /* MAC queue and bounded blocker diagnostics remain unchanged for legacy frames. */
    reset(); assert(ZbApp_SendCommand(0x99,(const uint8_t *)"x",1)==0);
    ZbApp.persist_tx_hold=1;ZbApp_TxTask();has_stage("BLOCK-PERSIST");
    unsigned printed=diagnostic_lines;ZbApp_TxTask();assert(diagnostic_lines==printed);
    now+=2001;ZbApp_TxTask();
#if (ZB_CONFIG_TX_DIAG_ENABLED != 0U)
    assert(diagnostic_lines==printed+1);
#endif
    ZbApp.persist_tx_hold=0;ZbApp.radio_exclusive=1;ZbApp_TxTask();has_stage("BLOCK-RADIO");
    ZbApp.radio_exclusive=0;ZbApp.tx_in_flight=3;ZbApp_TxTask();has_stage("BLOCK-INFLIGHT");
    assert(!submissions && ZbApp.tx_count==1);
    ZbApp.tx_in_flight=0;ZbApp_TxTask();assert(submissions==1 && !ZbApp.tx_count);
    reset();coordinator_endpoint=0;assert(ZbApp_SendCommand(0x99,(const uint8_t *)"x",1)==ZCL_STATUS_FAILURE);
    has_stage("REJECT-DEPENDENCY");
    reset();for(unsigned i=0;i<16;i++)assert(!ZbApp_SendCommand(0x99,(const uint8_t *)"x",1));
    assert(ZbApp_SendCommand(0x99,(const uint8_t *)"x",1)==ZCL_STATUS_INSUFFICIENT_SPACE);has_stage("REJECT-FULL");
    /* Jitter, 10-minute hard boundary and wraparound use actual standby code. */
    for(unsigned seed=1;seed<=4;seed++){
        MonitorStandby s={.random=seed};int awake;uint32_t start=UINT32_MAX-1000U;
        uint32_t first=monitor_standby_delay(&s,start,&awake);assert(awake && first>=3000 && first<=10000);
        uint32_t end=s.until;
        for(unsigned i=0;i<20;i++){uint32_t d=monitor_standby_delay(&s,start+i*10000U,&awake);assert(awake && d>=3000 && d<=10000 && s.until==end);}
        assert(monitor_standby_delay(&s,start+599999U,&awake)==1 && awake);
        uint32_t sleep=monitor_standby_delay(&s,start+600000U,&awake);assert(!awake && sleep>=285000 && sleep<=315000);
        monitor_standby_enabled(&s);sleep=monitor_standby_delay(&s,start+1,&awake);
        assert(!awake && sleep>=285000 && sleep<=315000);
    }
    reset();ZbApp_ScheduleMonitorStandby();assert(standby_blocked && poll_period==1000 && ZbConfigFastPoll);
    uint32_t deadline=monitor_standby.until;now+=10000;ZbApp_ScheduleMonitorStandby();assert(monitor_standby.until==deadline);
    now=deadline;ZbApp_ScheduleMonitorStandby();assert(!standby_blocked && !ZbConfigFastPoll && poll_period==250);
    assert(last_timer_delay>=285000 && last_timer_delay<=315000);
    reset();ZbApp.config_query_pending=1;ZbApp_ScheduleMonitorStandby();assert(!timer_calls);
    printf("Actual dual-mode control flow diagnostics=%u: no queries, jittered status/RX, timeout cleanup, applied ACK, duplicate ON, recovery guards and standby wraparound PASS\n",ZB_CONFIG_TX_DIAG_ENABLED);
    return 0;
}
