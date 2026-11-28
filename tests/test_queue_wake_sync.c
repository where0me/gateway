#include <assert.h>
#include <stdio.h>
#include "vibration_tx.h"
static uint8_t ZbUploadQueueSleeping,ZbUploadQueueWake;
static uint32_t ZbUploadQueueDeadlineRtc,rtc_now;
static struct {uint8_t upload_timer;} ZbApp;
static struct {uint32_t batch;} ZbDeferred;
static V2Tx ZbVibrationUpload;
static unsigned timers;
static uint32_t APP_ElapsedMs(void){return rtc_now;}
static void ZbApp_StartTimer(uint8_t id,uint32_t delay){(void)id;assert(delay);timers++;}
static void ZbApp_Log(const char *fmt,...){(void)fmt;}
static uint8_t ZbApp_ServiceQueueWake(uint32_t);
int main(void) {
    ZbUploadQueueSleeping=1;ZbUploadQueueDeadlineRtc=5000;
    assert(ZbApp_ServiceQueueWake(0) && !timers);
    ZbUploadQueueWake=1;rtc_now=1000;
    assert(ZbApp_ServiceQueueWake(1000) && timers==1);
    rtc_now=5000;ZbVibrationUpload.deferred=1;ZbVibrationUpload.phase=V2_TX_BATCH;
    assert(!ZbApp_ServiceQueueWake(5000) && !ZbUploadQueueSleeping && !ZbUploadQueueWake);
    assert(!ZbVibrationUpload.deferred && ZbVibrationUpload.deadline_ms==15000);
    assert(ZbVibrationUpload.phase==V2_TX_BATCH && timers==1);
    puts("Actual queue wake PASS: reapply directly at the queue deadline");
}
