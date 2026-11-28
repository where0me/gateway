#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "monitor_standby.h"
#define ZB_STATUS_SUCCESS 0
#define ZB_SLEEPY_PERSIST_AUTOSAVE_ENABLED 0
#define CFG_LPM_NODE_ADMISSION 7
#define CFG_LPM_APP_ZIGBEE 3
#define CFG_LPM_MONITOR_STANDBY 5
#define UTIL_LPM_ENABLE 1
#define UTIL_LPM_DISABLE 0
#define ZB_CONFIG_FLAG_MONITOR_PRESENT 4
#define ZB_CONFIG_FLAG_MONITOR_ENABLED 2
#define ZB_MONITOR_SLEEP 0
#define ZB_MONITOR_WAITING 1
static struct {unsigned join_status,commissioning_window_open,monitor_enabled,config_valid,monitor_state,config_query_pending,report_timer;} ZbApp;
static unsigned network_authorized,admission_expired,admission_phase,admission_timer_id=1,sampling_resume_pending,sampling_due,sampling_timer=2;
static struct {unsigned active;} sampling_cadence;
static MonitorStandby monitor_standby;
static unsigned blocked,pending_starts,pending_stops,stopped,connected=1;
static int APP_ZIGBEE_IsConnected(void){return connected;}
static void HW_TS_Stop(unsigned id){(void)id;stopped++;}
static void UTIL_LPM_SetStopMode(unsigned mask,unsigned enabled){if(enabled)blocked&=~mask;else blocked|=mask;}
/* Pending nodes keep the receiver active and arm the link watchdog. */
static void PendingLink_Start(void){pending_starts++;}
static void PendingLink_Stop(void){pending_stops++;}
static void ZbApp_Log(const char *s,...){(void)s;}
static void ZbApp_CheckAuthorizationBudget(void);
static uint8_t ZbApp_ApplyMonitorSwitch(uint8_t flags);
static void pending_standby(void);
int main(void)
{
    admission_phase=2;ZbApp_CheckAuthorizationBudget();
    assert(!network_authorized && !admission_phase && pending_starts && (blocked&(1U<<7)));
    for(unsigned i=0;i<100;i++) {
        ZbApp_CheckAuthorizationBudget();pending_standby();
        assert((blocked&(1U<<7)) && !network_authorized);
    }
    ZbApp.config_valid=1;ZbApp_ApplyMonitorSwitch(4);ZbApp_CheckAuthorizationBudget();
    assert(!network_authorized && (blocked&(1U<<7))); /* OFF config never grants */
    sampling_resume_pending=1;sampling_cadence.active=1;
    ZbApp_ApplyMonitorSwitch(6);ZbApp_CheckAuthorizationBudget();
    assert(network_authorized && !(blocked&(1U<<7)) && !sampling_resume_pending && !sampling_cadence.active && pending_stops);
    ZbApp.monitor_enabled=0;sampling_resume_pending=1;ZbApp_ApplyMonitorSwitch(6);
    assert(sampling_resume_pending); /* Existing members retain reconnect schedule. */
    ZbApp_ApplyMonitorSwitch(4);ZbApp_CheckAuthorizationBudget();
    assert(!network_authorized && (blocked&(1U<<7))); /* Gateway lost/replaced its DB. */
    puts("Node manual admission PASS: pending never enters STOP2 or auto authorizes, no periodic standby timer, first grant immediate, reconnect cadence retained");
}
