#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vibration_tx.h"
#include "upload_lease.h"
#include "link_health.h"
#define CFG_ZIGBEE_NVM_ENABLED 1
#define ZB_SLEEPY_PERSIST_AUTOSAVE_ENABLED 1
#define ZB_PERSIST_DATA_OFFSET 12U
#define CFG_LPM_APP_ZIGBEE 1U
#define UTIL_LPM_DISABLE 0U
#define UTIL_LPM_ENABLE 1U
#define CFG_TASK_ZIGBEE_UPLOAD 2U
#define CFG_SCH_PRIO_0 0U
enum ZbStatusCodeT {ZB_STATUS_SUCCESS=0,ZB_STATUS_ALLOC_FAIL=0x87};
static struct {
    void *zb;
    uint8_t nvm_ready,join_blocked_until_reset,startup_wait_active,restore_pending,radio_exclusive;
    uint8_t persist_tx_hold,audio_waiting,join_retry_timer,report_timer,persist_timer;
    uint8_t persist_save_pending,config_query_pending,upload_allowed,parent_sync_pending;
    uint8_t rejoin_probe_pending,rejoin_probe_rtc,rtc_join_active,rtc_join_attempts,restore_recovery_active;
    uint8_t parent_online,parent_state_known,rejoin_via_startup;
    enum ZbStatusCodeT join_status;
} ZbApp;
static uint8_t network_authorized,leave_cleanup_pending,ZbSamplingCapture,hy_zdo_inflight,hy_parent_response;
static uint8_t recovery_announce_pending,ZbUploadQueueSleeping,persist_restore_inflight;
static uint32_t persist_restore_generation,persist_restore_request_generation,upload_generation;
static LinkHealth upload_health;
static UploadLease upload_lease;
static V2Tx ZbVibrationUpload;
static union {uint8_t bytes[1024];uint32_t words[256];} ZbPersistCache;
static struct {uint8_t busy;} ZbUploadSlots[3];
static uint8_t flash_records[8],ble_active,monitor_enabled;
static uint32_t now,current_counter,saved_counter;
static unsigned saves,resets,restores,stops,tx_resets,persistence_notifications,tasks,power_calls;
static int save_ok,transmitting,synchronous_callback,revoke_during_save;
static enum ZbStatusCodeT submit_status;
static void (*restore_callback)(enum ZbStatusCodeT,void *);
static void *restore_arg;
static uint32_t APP_ElapsedMs(void){return now;}
static int APP_ZIGBEE_IsTransmitting(void){return transmitting;}
static bool ZbApp_PersistSave(void) {
    saves++;assert(ZbApp.startup_wait_active);
    if(!save_ok)return false;
    saved_counter=current_counter+2048U;
    memset(&ZbPersistCache,0,sizeof(ZbPersistCache));ZbPersistCache.words[0]=32;
    memcpy(ZbPersistCache.bytes+12,&saved_counter,4);
    if(revoke_during_save){network_authorized=0;leave_cleanup_pending=1;}return true;
}
static void ZbApp_StopUploadFastPoll(void){stops++;}
static void ZbApp_StopConfigTransaction(void){stops++;}
static void ZbApp_StopAdmissionFastPoll(void){stops++;}
static void HW_TS_Stop(uint8_t timer){(void)timer;stops++;}
static void ZbApp_ResetTxQueue(void){tx_resets++;}
static void ZbApp_ClearDeferredUpload(void){} // Unprotects, never acknowledges/erases Flash records.
static void UTIL_LPM_SetStopMode(uint32_t mask,unsigned mode){(void)mask;(void)mode;}
static void ZbApp_SetMaximumTxPower(void){power_calls++;}
static void ZbReset(void *zb){assert(zb==&ZbApp && saves && save_ok);resets++;}
static void ZbApp_Log(const char *fmt,...){(void)fmt;}
static void UTIL_SEQ_SetTask(uint32_t mask,unsigned priority){(void)priority;tasks|=mask;}
static void ZbApp_EnablePersistence(void){persistence_notifications++;}
static enum ZbStatusCodeT ZbStartupPersist(void *zb,const void *data,unsigned length,void *cbke,
    void (*callback)(enum ZbStatusCodeT,void *),void *arg)
{
    (void)cbke;assert(zb==&ZbApp && resets && ZbApp.restore_pending && persist_restore_inflight);
    assert(length==32 && data==ZbPersistCache.bytes+12);
    uint32_t counter;memcpy(&counter,data,4);assert(counter==saved_counter && counter>current_counter);
    restores++;restore_callback=callback;restore_arg=arg;
    if(synchronous_callback && submit_status==ZB_STATUS_SUCCESS)callback(ZB_STATUS_SUCCESS,arg);
    return submit_status;
}
static void ZbApp_RuntimeRestoreCallback(enum ZbStatusCodeT,void *);
static int ZbApp_RestartSavedNetwork(void);
static void reset(void) {
    memset(&ZbApp,0,sizeof(ZbApp));memset(&upload_health,0,sizeof(upload_health));
    memset(ZbUploadSlots,0,sizeof(ZbUploadSlots));memset(&upload_lease,0,sizeof(upload_lease));
    ZbApp.zb=&ZbApp;ZbApp.nvm_ready=1;network_authorized=1;
    leave_cleanup_pending=ZbSamplingCapture=transmitting=persist_restore_inflight=0;
    saves=resets=restores=stops=tx_resets=persistence_notifications=tasks=power_calls=0;
    synchronous_callback=revoke_during_save=0;submit_status=ZB_STATUS_SUCCESS;save_ok=1;
    now=120000;current_counter=2622;ble_active=monitor_enabled=1;
    memset(flash_records,0xa5,sizeof(flash_records));upload_health.pending=1;
}
int main(void) {
    reset();save_ok=0;assert(ZbApp_RestartSavedNetwork()==-1 && saves==1 && !resets && !restores);
    reset();ZbApp.radio_exclusive=1;assert(!ZbApp_RestartSavedNetwork() && !saves);
    ZbApp.radio_exclusive=0;ZbSamplingCapture=1;assert(!ZbApp_RestartSavedNetwork() && !saves);
    ZbSamplingCapture=0;ZbUploadSlots[1].busy=1;assert(!ZbApp_RestartSavedNetwork() && !saves);
    ZbUploadSlots[1].busy=0;persist_restore_inflight=1;assert(!ZbApp_RestartSavedNetwork() && !saves);
    persist_restore_inflight=0;ZbApp.nvm_ready=0;assert(ZbApp_RestartSavedNetwork()==-1 && !resets);
    reset();ZbApp.config_query_pending=ZbApp.persist_save_pending=1;
    upload_lease.valid=1;ZbUploadQueueSleeping=1;
    assert(ZbApp_RestartSavedNetwork()==1);
    assert(resets==1 && restores==1 && saves==1 && power_calls==1 && tx_resets==1);
    assert(ZbApp.restore_pending && !ZbApp.startup_wait_active && !upload_lease.valid);
    assert(!ZbUploadQueueSleeping && !ZbApp.config_query_pending);
    assert(ble_active && monitor_enabled && network_authorized && flash_records[0]==0xa5);
    ZbApp_RuntimeRestoreCallback(ZB_STATUS_SUCCESS,(void *)(uintptr_t)(persist_restore_generation-1));
    assert(ZbApp.restore_pending); // Retired callback cannot finish this reset.
    restore_callback(ZB_STATUS_SUCCESS,restore_arg);
    assert(!ZbApp.restore_pending && !persist_restore_inflight && !upload_health.verifying);
    assert(ZbApp.rejoin_probe_pending && ZbApp.rtc_join_active && !ZbApp.parent_online);
    assert(persistence_notifications==1 && upload_health.pending && tasks);
    reset();submit_status=ZB_STATUS_ALLOC_FAIL;
    assert(ZbApp_RestartSavedNetwork()==1 && !ZbApp.restore_pending && upload_health.backoff);
    assert(upload_health.deadline==now+60000 && upload_health.rounds==2 && network_authorized);
    reset();synchronous_callback=1;assert(ZbApp_RestartSavedNetwork()==1 && !ZbApp.restore_pending);
    reset();revoke_during_save=1;
    assert(ZbApp_RestartSavedNetwork()==-1 && !resets && !ZbApp.startup_wait_active);
    reset();assert(ZbApp_RestartSavedNetwork()==1);network_authorized=0;leave_cleanup_pending=1;
    restore_callback(ZB_STATUS_SUCCESS,restore_arg);
    assert(!ZbApp.parent_online && !ZbApp.rejoin_probe_pending && !persistence_notifications);
    puts("Actual runtime reset PASS: fresh counter snapshot before APS/NWK reset; busy/BLE/TX/restore guards; Flash/policy/credentials retained; late/immediate/failed callbacks and verification gate");
}
