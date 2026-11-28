#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "report_dedup.h"
#include "monitor_report.h"
static ReportDedup cache;
int main(void) {
    const char *mac="01:02:03:04:05:06:07:08",*other="11:12:13:14:15:16:17:18";
    uint8_t state=3,a[32]={2,0x34,0x12,1},b[32]={2,0x34,0x12,2};
    assert(!report_seen(&cache,mac,42,0x8d,&state,1,100,0));
    report_remember(&cache,mac,42,0x8d,&state,1,100);
    assert(report_seen(&cache,mac,42,0x8d,&state,1,200,0));
    assert(!report_seen(&cache,other,42,0x8d,&state,1,200,0));
    assert(!report_seen(&cache,mac,43,0x8d,&state,1,200,0));
    state=4;report_remember(&cache,mac,42,0x8d,&state,1,300);
    state=3;assert(report_seen(&cache,mac,42,0x8d,&state,1,400,0)); /* delayed replay */
    assert(monitor_report_replay(&cache,mac,42,MONITOR_STATE_UPLOADING)); /* COMPLETE is terminal */
    assert(monitor_report_replay(&cache,mac,42,MONITOR_STATE_ERROR));
    state=MONITOR_STATE_UPLOADING;report_remember(&cache,mac,43,0x8d,&state,1,400);
    assert(monitor_report_replay(&cache,mac,43,state));
    assert(monitor_report_replay(&cache,mac,43,MONITOR_STATE_CAPTURING));
    assert(!monitor_report_replay(&cache,mac,43,MONITOR_STATE_ERROR));
    state=MONITOR_STATE_ERROR;report_remember(&cache,mac,43,0x8d,&state,1,500);
    assert(!monitor_report_replay(&cache,mac,43,MONITOR_STATE_UPLOADING)); /* real retry is visible */
    state=3;
    report_remember(&cache,mac,0,0x99,a,sizeof(a),1000);
    assert(report_latest_seen(&cache,mac,0,0x99,a,sizeof(a),9999,10000));
    assert(!report_latest_seen(&cache,mac,0,0x99,a,sizeof(a),11000,10000));
    assert(!report_latest_seen(&cache,mac,0,0x99,b,sizeof(b),1001,10000));
    report_remember(&cache,mac,0,0x99,b,sizeof(b),1001);
    assert(!report_latest_seen(&cache,mac,0,0x99,a,sizeof(a),1002,10000));
    report_remember(&cache,mac,0,0x99,a,sizeof(a),1002); /* configuration A -> B -> A */
    assert(report_latest_seen(&cache,mac,0,0x99,a,sizeof(a),1003,10000));
    report_remember(&cache,mac,42,0x92,NULL,0,2000);
    assert(report_seen(&cache,mac,42,0x92,NULL,0,3000,10000));
    assert(!report_seen(&cache,mac,42,0x92,NULL,0,63000,10000)); /* real queue wake */
    report_forget(&cache,mac);
    assert(!report_seen(&cache,mac,42,0x8d,&state,1,4000,0));
    for(unsigned i=0;i<REPORT_DEDUP_CAPACITY+10;i++)report_remember(&cache,mac,i,1,a,sizeof(a),i);
    assert(!report_seen(&cache,mac,0,1,a,sizeof(a),0,0));
    assert(report_seen(&cache,mac,REPORT_DEDUP_CAPACITY+9,1,a,sizeof(a),0,0));
    puts("Report replay cache: batch/state isolation, delayed replay, retry burst, config changes and bounded eviction PASS");
    return 0;
}
