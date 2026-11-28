#ifndef MONITOR_REPORT_H
#define MONITOR_REPORT_H
#include "monitor_protocol.h"
#include "report_dedup.h"
/* Business states only. Idle heartbeats must continue updating telemetry.
 * COMPLETE is terminal for a batch; ERROR -> UPLOADING is a genuine retry.
 * A batch cannot return to acquisition (a new acquisition has a new ID). */
static inline int monitor_report_replay(const ReportDedup *cache,const char *mac,
                                        uint32_t batch,uint8_t state) {
    const ReportDedupEntry *last=report_latest_entry(cache,mac,batch,GW_RSP_MONITOR_STATE);
    if(!last || last->length!=1)return 0;
    return last->payload[0]==state || last->payload[0]==MONITOR_STATE_COMPLETE ||
           state==MONITOR_STATE_CAPTURING;
}
#endif
