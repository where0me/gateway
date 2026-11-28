#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "control_sync.h"
#include "sampling_cadence.h"
#define V2_BATCH_LEN 17
#define CFG_TASK_ZIGBEE_UPLOAD 1
#define CFG_TASK_ZIGBEE_REPORT 2
#define CFG_SCH_PRIO_0 0
#define CFG_SCH_PRIO_1 1
#define ZB_CONFIG_QUERY_JOIN 1
static struct {
    unsigned monitor_enabled,upload_allowed,audio_waiting,config_query_pending;
    struct {uint32_t auto_interval_sec;} monitor_config;
} ZbApp;
static SamplingCadence sampling_cadence;
static uint8_t sampling_resume_pending,sampling_due,sampling_timer=7,ZbSamplingCapture;
static uint32_t now,armed_delay,task_mask;
static unsigned stopped,windows,backlog;
static int connected=1,flash_pending=1;
static int ZigbeeVibrationStore_Pending(uint8_t *p){(void)p;return flash_pending;}
static void ZbApp_ResumeCachedUpload(void);
static uint32_t APP_ElapsedMs(void){return now;}
static int APP_ZIGBEE_IsConnected(void){return connected;}
static void HW_TS_Stop(uint8_t timer){assert(timer==7);stopped++;}
static void ZbApp_StartTimer(uint8_t timer,uint32_t delay){assert(timer==7);armed_delay=delay;}
static void ZbApp_Log(const char *fmt,...){(void)fmt;}
static void UTIL_SEQ_SetTask(uint32_t mask,unsigned prio){(void)prio;task_mask|=mask;}
static void ZbApp_OpenControlWindow(uint8_t reason){assert(reason==1);windows++;ZbApp.config_query_pending=1;}
static void ZbApp_SendPendingSnapshot(void){backlog++;}
static void ZbApp_ArmSamplingTimer(void);
static void ZbApp_RequireSamplingSchedule(void);
static uint8_t ZbApp_ApplySamplingDeadline(uint32_t,uint32_t);
static uint8_t ZbApp_WaitForSamplingDeadline(uint8_t);
int main(void)
{
    const uint32_t epoch=1793693100U;
    ZbApp.monitor_enabled=1;ZbApp.monitor_config.auto_interval_sec=300;
    sampling_cadence_start(&sampling_cadence,0,300000);now=125000;
    ZbApp_RequireSamplingSchedule();assert(sampling_resume_pending && stopped==1);
    assert(ZbApp_WaitForSamplingDeadline(0));
    assert(windows==1 && backlog==1 && ZbApp.upload_allowed && task_mask==(1U<<CFG_TASK_ZIGBEE_UPLOAD));
    assert(ZbApp_ApplySamplingDeadline(epoch,175000));
    assert(!sampling_resume_pending && sampling_cadence.next==300000 && armed_delay==175000);
    now=130000;assert(!ZbApp_ApplySamplingDeadline(epoch,175000));
    assert(sampling_cadence.next==300000); /* Delayed duplicate cannot push the deadline. */
    ZbApp.config_query_pending=0;
    assert(ZbApp_WaitForSamplingDeadline(0) && armed_delay==170000 && windows==1);
    assert(ZbApp_WaitForSamplingDeadline(0) && backlog>=3);
    now=300000;assert(!ZbApp_WaitForSamplingDeadline(1));
    assert(sampling_cadence_due(&sampling_cadence,now)==1 && sampling_cadence.next==600000);
    assert(ZbApp_WaitForSamplingDeadline(0)); /* Unrelated REPORT cannot recapture. */
    ZbApp.audio_waiting=1;assert(!ZbApp_WaitForSamplingDeadline(0));ZbApp.audio_waiting=0;
    sampling_cadence.active=0;assert(!ZbApp_WaitForSamplingDeadline(0)); /* First user enable remains immediate. */
    now=UINT32_MAX-10000;ZbApp_RequireSamplingSchedule();
    assert(ZbApp_ApplySamplingDeadline(epoch,15000));
    now+=14999;assert(ZbApp_WaitForSamplingDeadline(0));now++;
    assert(sampling_cadence_due(&sampling_cadence,now)==1);
    ZbApp_RequireSamplingSchedule();ZbApp.monitor_enabled=0;
    assert(!ZbApp_ApplySamplingDeadline(epoch,1000) && sampling_resume_pending);
    ZbApp.monitor_enabled=1;
    assert(ZbApp_ApplySamplingDeadline(epoch,350000));
    assert(sampling_cadence.next==now+350000 && sampling_cadence.period==300000);
    flash_pending=0;ZbApp.upload_allowed=0;task_mask=0;
    ZbApp_ResumeCachedUpload();assert(!ZbApp.upload_allowed && !task_mask);
    flash_pending=1;ZbApp_ResumeCachedUpload();assert(ZbApp.upload_allowed && task_mask);
    assert(control_resume_delay(15000,2000,300000)==13000);
    assert(control_resume_delay(15000,15000,300000)==300000);
    assert(control_resume_delay(15000,620000,300000)==295000);
    puts("Actual node reconnect sampling: future deadline, no duplicate capture, backlog upload before due, no catch-up, first enable, distinct planned delay and RTC wrap PASS");
}
