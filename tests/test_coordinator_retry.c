#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "control_retry.h"
#define CHILD_SLOTS 2
static struct {uint8_t used,ieee[8],hy;uint16_t address;} children[CHILD_SLOTS];
static ControlRetry controlRetries[CHILD_SLOTS];
static uint8_t controlIdentity[CHILD_SLOTS][8];
static int controlSendingChild=-1;
static unsigned submitted,diagnostics,emitted;
static uint8_t next_status,last_payload[CONTROL_RESUME_CONFIG_SIZE];
static uint16_t last_address;
static void Gateway_Put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void emit(uint8_t cmd,const uint8_t *p,uint8_t n){assert(cmd==0x8e && n==4 && p[2]==0x0c);emitted++;}
static void diagnostic(uint8_t event,const uint8_t *ieee,uint16_t address,uint8_t a,uint8_t b,uint8_t c,uint8_t d,uint32_t value)
{(void)ieee;(void)address;(void)b;(void)c;(void)d;(void)value;assert(event==0x37 && a==0x0c);diagnostics++;}
static uint8_t nodeCommand(uint16_t addr,uint8_t cmd,const uint8_t *p,uint8_t n,bool track)
{assert(controlSendingChild>=0 && cmd==0x0c && (n==29 || n==CONTROL_RESUME_CONFIG_SIZE) && track);last_address=addr;memcpy(last_payload,p,n);submitted++;return next_status;}
static void controlAttempt(unsigned,uint32_t);
static void controlApplied(unsigned,const uint8_t *,unsigned);
static void report_for(uint8_t report[8],const uint8_t config[29])
{
    memset(report,0,8);report[0]=CONTROL_STATUS_VERSION;
    report[1]=CONTROL_VALID | ((config[26]&2U)?CONTROL_ENABLED:0);
    uint32_t hash=control_fingerprint(config,29);
    for(unsigned i=0;i<4;i++)report[2+i]=(uint8_t)(hash>>(8*i));
}
int main(void) {
    children[0].used=children[0].hy=1;children[0].ieee[0]=7;children[0].address=123;
    memcpy(controlIdentity[0],children[0].ieee,8);
    uint8_t on[29]={1},off[29]={0};ControlRetry *r=&controlRetries[0];
    assert(control_retry_replace(r,on,29,0));next_status=0x10;controlAttempt(0,0);
    assert(submitted==1 && !r->inflight && r->active && emitted==1 && diagnostics==2);
    controlAttempt(0,r->next-1);assert(submitted==1);
    next_status=0;controlAttempt(0,r->next);assert(submitted==2 && r->inflight);
    uint32_t old=r->generation;control_retry_replace(r,off,29,r->next+1);
    controlAttempt(0,r->next+1);assert(submitted==2); // one actual stack transaction per node
    control_retry_result(r,old,0,r->next+2);children[0].address=456;
    controlAttempt(0,r->next+2);assert(submitted==3 && !last_payload[0] && last_address==456);
    control_retry_result(r,r->generation,0,10000);controlAttempt(0,20000);assert(submitted==3);
    control_retry_replace(r,on,29,21000);children[0].ieee[0]=8;controlAttempt(0,21000);
    assert(submitted==3 && !r->active && controlSendingChild==-1); // recycled child cannot inherit old command
    memcpy(controlIdentity[0],children[0].ieee,8);
    on[26]=6;off[26]=4;
    control_retry_replace(r,on,29,22000);controlAttempt(0,22000);
    assert(submitted==4 && r->active && r->inflight);
    uint8_t report[8];report_for(report,on);
    report[0]=2;controlApplied(0,report,8);assert(r->active);
    report_for(report,on);controlApplied(0,report,7);assert(r->active);
    report[1]&=(uint8_t)~CONTROL_VALID;controlApplied(0,report,8);assert(r->active);
    report_for(report,on);report[1]&=(uint8_t)~CONTROL_ENABLED;controlApplied(0,report,8);assert(r->active);
    report_for(report,off);controlApplied(0,report,8);assert(r->active);
    report_for(report,on);children[0].ieee[0]^=1;controlApplied(0,report,8);assert(r->active);
    children[0].ieee[0]^=1;
    /* Valid current-state heartbeat also confirms a lost explicit APPLIED reply. */
    controlApplied(0,report,8);assert(!r->active && r->applied && r->inflight);
    unsigned previous_diagnostics=diagnostics;
    controlApplied(0,report,8);assert(diagnostics==previous_diagnostics);
    controlAttempt(0,100000);assert(submitted==4); /* Cannot retry after application proof. */
    old=r->generation;
    control_retry_replace(r,off,29,23000);
    assert(r->active && !r->applied && r->inflight);
    controlApplied(0,report,8);assert(r->active); // stale ON cannot confirm newest OFF
    control_retry_result(r,old,0xf0,24000); // late old failure only frees its AF slot
    assert(r->active && !r->inflight && !r->applied);
    controlAttempt(0,24000);assert(submitted==5 && r->inflight);
    report_for(report,off);report[1]|=CONTROL_APPLIED;controlApplied(0,report,8);
    assert(!r->active && r->applied && r->inflight);
    control_retry_result(r,r->generation,0xf0,25000);
    assert(!r->active && !r->inflight);
    controlAttempt(0,100000);assert(submitted==5);
    /* Matching application confirmation cancels an already scheduled retry too. */
    control_retry_replace(r,on,29,26000);controlAttempt(0,26000);
    control_retry_result(r,r->generation,0xf0,27000);assert(r->active && !r->inflight);
    report_for(report,on);controlApplied(0,report,8);controlAttempt(0,100000);
    assert(submitted==6 && !r->active && r->applied);
    control_retry_schedule(r,off,29,CONTROL_DELIVERY_CACHE,300000);
    controlAttempt(0,1000000);assert(submitted==6 && !r->active);
    control_retry_schedule(r,off,29,CONTROL_DELIVERY_WINDOW,1000000);
    controlAttempt(0,1000000);assert(submitted==7 && r->inflight);
    control_retry_result(r,r->generation,0xf0,1006500);
    controlAttempt(0,2000000);assert(submitted==7 && !r->active);
    control_retry_schedule(r,off,29,CONTROL_DELIVERY_WINDOW,2000000);
    controlAttempt(0,2000000);assert(submitted==8 && r->inflight);
    report_for(report,off);controlApplied(0,report,8);
    control_retry_result(r,r->generation,0xf0,2001000);
    controlAttempt(0,3000000);assert(submitted==8 && r->applied);
    puts("Actual coordinator controlAttempt: stack rejection -> jitter retry, no duplicate inflight, latest OFF replaces ON, moved address, removed/reused identity PASS");
    puts("Actual coordinator controlApplied: identity/hash/switch validation, stale status, queued/inflight cancellation, late AF failure and newer intent PASS");
    puts("Actual coordinator scoped config: cache sends none; window expiry stops radio retries; next wake reopens; applied proof stops retries PASS");
}
