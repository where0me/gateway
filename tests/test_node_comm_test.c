#include <assert.h>
#include <stdint.h>
#include "link_health.h"
#include "monitor_standby.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define CFG_TASK_ZIGBEE_COMM_TEST 20
#define CFG_SCH_PRIO_1 1
#define UTIL_SEQ_RFU 0
#define CFG_TIM_PROC_ID_ZIGBEE 1
#define hw_ts_SingleShot 0
#define hw_ts_Successful 0
#define ZB_APSDE_ADDRMODE_SHORT 2
#define ZB_STATUS_SUCCESS 0
#define ZB_APS_STATUS_NO_ACK 0xa7
enum ZclStatusCodeT {ZCL_STATUS_SUCCESS=0,ZCL_STATUS_FAILURE=1};
struct ZbZclCommandRspT {unsigned aps_status,status;};
struct ZbZclClusterCommandReqT {struct{unsigned mode,nwkAddr,endpoint;}dst;unsigned cmdId,noDefaultResp,length;const uint8_t *payload;};
static struct {void *zb,*control_server;unsigned join_status,parent_online,config_valid,monitor_enabled,
    join_blocked_until_reset,radio_exclusive,persist_tx_hold,startup_wait_active,rtc_join_active,
    restore_pending,restore_recovery_active,audio_waiting,tx_in_flight,tx_count;} ZbApp;
static struct {unsigned held;} radio_sleep;
static int ZbVibrationUpload;static int v2_tx_active(const int *upload){assert(upload==&ZbVibrationUpload);return 0;}
static unsigned network_authorized,coordinator_endpoint,ZbSamplingCapture;
static struct {unsigned pending,failures;}upload_health;
static unsigned admission_expired, recovery_calls;
static MonitorStandby monitor_standby = {.random=123};
static int APP_ZIGBEE_IsPendingAuthorization(void)
{
    return ZbApp.zb && ZbApp.parent_online && !network_authorized &&
           !ZbApp.join_status && !ZbApp.join_blocked_until_reset;
}
static void ZbApp_SetParentOnline(unsigned online, unsigned status)
{
    assert(!online && status == ZB_APS_STATUS_NO_ACK);
    ZbApp.parent_online = online;
}
static void ZbApp_StartLinkRecovery(void) {recovery_calls++;}
static uint32_t clock_ms;static unsigned sends,stops,starts;static int submit_error,timer_error;
static char logs[16384];
static void ZbApp_Log(const char *fmt,...)
{va_list ap;va_start(ap,fmt);vsnprintf(logs+strlen(logs),sizeof(logs)-strlen(logs),fmt,ap);va_end(ap);}
static uint32_t APP_ElapsedMs(void){return clock_ms;}
static void UTIL_SEQ_RegTask(unsigned mask,unsigned priority,void (*fn)(void)){(void)mask;(void)priority;(void)fn;}
static void UTIL_SEQ_SetTask(unsigned mask,unsigned priority){(void)mask;(void)priority;}
static int HW_TS_Create(unsigned proc,uint8_t *timer,unsigned mode,void (*fn)(void))
{static uint8_t next=8;(void)proc;(void)mode;(void)fn;*timer=next++;return timer_error;}
static void HW_TS_Stop(unsigned timer){assert(timer==8 || timer==9);if(timer==8)stops++;}
static void ZbApp_StartTimer(unsigned timer,unsigned ms)
{
    assert(timer==8 || (timer==9 && ms>0 && ms<=45000));
    if(timer==8){assert(ms>0 && ms<=10000);starts++;}
}
static uint8_t sent_payload[5];static void (*confirm_cb)(struct ZbZclCommandRspT*,void*);static void *confirm_arg;
static enum ZclStatusCodeT ZbZclClusterCommandReq(void *cluster,struct ZbZclClusterCommandReqT *r,
    void (*cb)(struct ZbZclCommandRspT*,void*),void *arg)
{
    assert(cluster==ZbApp.control_server && r->dst.nwkAddr==0 && r->dst.endpoint==8);
    assert(r->cmdId==0xB0 && r->length==5 && r->noDefaultResp==0);
    memcpy(sent_payload,r->payload,5);sends++;confirm_cb=cb;confirm_arg=arg;
    return submit_error?ZCL_STATUS_FAILURE:ZCL_STATUS_SUCCESS;
}
struct ZbApsdeDataConfT {unsigned status;};
static void (*aps_cb)(struct ZbApsdeDataConfT *, void *);
static void *aps_arg;
static unsigned ZbApp_SendPendingLinkFrame(uint8_t command, const uint8_t *payload,
    uint8_t length, void (*callback)(struct ZbApsdeDataConfT *, void *), void *arg)
{
    assert(command==0xb0 && length==sizeof(sent_payload));
    memcpy(sent_payload,payload,length);
    sends++;aps_cb=callback;aps_arg=arg;
    return submit_error ? 1U : 0U;
}
#include "node_comm_test.h"
static unsigned char saved_app[sizeof(ZbApp)],saved_health[sizeof(upload_health)];
static void snapshot(void){memcpy(saved_app,&ZbApp,sizeof(ZbApp));memcpy(saved_health,&upload_health,sizeof(upload_health));}
static void unchanged(void){assert(!memcmp(saved_app,&ZbApp,sizeof(ZbApp)) && !memcmp(saved_health,&upload_health,sizeof(upload_health)));
    assert(network_authorized==0 && coordinator_endpoint==8 && !ZbSamplingCapture);}
int main(void)
{
    ZbApp.zb=(void*)1;ZbApp.control_server=(void*)2;ZbApp.parent_online=1;ZbApp.config_valid=1;
    coordinator_endpoint=8;upload_health.failures=3;snapshot();
    APP_ZIGBEE_CommTest();assert(!sends && strstr(logs,"NOT READY"));unchanged();
    CommTest_Init(42);
    radio_sleep.held=1;APP_ZIGBEE_CommTest();assert(!sends && !comm_test.active);unchanged();
    radio_sleep.held=0;APP_ZIGBEE_CommTest();assert(sends==1 && starts==1 && comm_test.active);unchanged();
    APP_ZIGBEE_CommTest();assert(sends==1 && strstr(logs,"BUSY"));unchanged();
    uint8_t echo[7];memcpy(echo,sent_payload,5);echo[5]=(uint8_t)-47;echo[6]=156;
    echo[1]^=1;CommTest_Reply(echo,7);assert(comm_test.active);echo[1]^=1;
    CommTest_Reply(echo,6);assert(comm_test.active);
    struct ZbZclCommandRspT failure={0xe9,1};confirm_cb(&failure,confirm_arg);
    assert(comm_test.active && strstr(logs,"no MAC receive acknowledgement"));unchanged();
    clock_ms=100;CommTest_Reply(echo,7);assert(!comm_test.active && stops==1 && strstr(logs,"PASS") && strstr(logs,"RTT=100"));unchanged();
    CommTest_Reply(echo,7);assert(stops==1); /* duplicate cannot apply */
    APP_ZIGBEE_CommTest();assert(sends==2 && comm_test.active);
    CommTest_Reply(echo,7);assert(comm_test.active); /* previous test echo */
    clock_ms+=10000;memcpy(echo,sent_payload,5);CommTest_Reply(echo,7);assert(comm_test.active);
    CommTest_Task();assert(!comm_test.active && strstr(logs,"TIMEOUT"));unchanged();
    submit_error=1;APP_ZIGBEE_CommTest();assert(!comm_test.active && starts==2);unchanged();submit_error=0;
    ZbApp.radio_exclusive=1;snapshot();unsigned previous=sends;APP_ZIGBEE_CommTest();assert(sends==previous);unchanged();
    ZbApp.radio_exclusive=0;ZbApp.parent_online=0;snapshot();
    clock_ms=UINT32_MAX-100U;APP_ZIGBEE_CommTest();assert(comm_test.active);clock_ms+=10000U;CommTest_Task();
    assert(!comm_test.active);unchanged(); /* wrap + failure never set parent online */
    puts("Manual communication test PASS");

    ZbApp.parent_online=1;
    PendingLink_Start();
    assert(pending_link.active && pending_link.deadline-clock_ms>=30000 &&
           pending_link.deadline-clock_ms<=45000);
    clock_ms+=10000;
    PendingLink_RecordSuccess(); /* a successfully confirmed gateway probe reply */
    uint32_t deadline=pending_link.deadline;
    previous=sends;
    clock_ms=deadline-1;CommTest_Task();assert(sends==previous);
    clock_ms=deadline;ZbApp.tx_count=1;CommTest_Task();
    assert(sends==previous && !pending_link.failures);
    clock_ms=pending_link.deadline;ZbApp.tx_count=0;submit_error=1;CommTest_Task();
    assert(!pending_link.failures && !comm_test.active);submit_error=0;
    clock_ms=pending_link.deadline;CommTest_Task();
    assert(comm_test.automatic && comm_test.active);
    struct ZbApsdeDataConfT local_error={0xe1}, peer_failure={0xe9}, peer_success={0};
    aps_cb(&local_error,aps_arg);
    clock_ms+=10000;CommTest_Task();assert(!pending_link.failures && !recovery_calls);

    clock_ms=pending_link.deadline;CommTest_Task();
    void *previous_token=aps_arg;
    aps_cb(&peer_failure,aps_arg);
    clock_ms+=10000;CommTest_Task();assert(pending_link.failures==1 && !recovery_calls);

    APP_ZIGBEE_CommTest();assert(comm_test.active && !comm_test.automatic);
    confirm_cb(&failure,confirm_arg);
    clock_ms+=10000;CommTest_Task();assert(pending_link.failures==1 && !recovery_calls);

    clock_ms=pending_link.deadline;CommTest_Task();
    aps_cb(&peer_failure,previous_token);assert(!comm_test.peer_result);
    memcpy(echo,sent_payload,5);clock_ms+=200;CommTest_Reply(echo,7);
    assert(!comm_test.active && !pending_link.failures && !recovery_calls);

    clock_ms=pending_link.deadline;CommTest_Task();assert(comm_test.active);
    PendingLink_RecordSuccess(); /* cancels automatic echo only */
    assert(!comm_test.active && !pending_link.failures);
    for(unsigned i=0;i<2;i++){
        clock_ms=pending_link.deadline;CommTest_Task();assert(comm_test.active);
        aps_cb(i==0 ? &peer_failure : &peer_success,aps_arg);
        clock_ms+=10000;CommTest_Task();
    }
    assert(recovery_calls==1 && !pending_link.active && !ZbApp.parent_online &&
           !network_authorized && ZbApp.config_valid);
    ZbApp.parent_online=1;
    PendingLink_Start();assert(pending_link.active);
    clock_ms=pending_link.deadline;CommTest_Task();assert(comm_test.active);
    network_authorized=1;CommTest_Task();assert(!pending_link.active && !comm_test.active);
    clock_ms+=10000;CommTest_Task();assert(recovery_calls==1);
    puts("Pending link watchdog PASS");
}
