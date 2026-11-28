#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static struct {void *zb;} ZbApp;
static uint32_t clock_ms;static uint32_t APP_ElapsedMs(void){return clock_ms;}
static uint16_t ZbShortAddress(void *zb){(void)zb;return 0x714C;}
static char output[16000];static size_t output_length;
static void ZbApp_Log(const char *fmt,...){
    va_list ap;va_start(ap,fmt);int n=vsnprintf(output+output_length,sizeof(output)-output_length,fmt,ap);va_end(ap);
    assert(n>=0 && (size_t)n<sizeof(output)-output_length);output_length+=(size_t)n;
}
#include "node_upload_trace.h"
int main(void){
    uint8_t data[7]={42,0,0,0,11,0,19};
    ZbApp_TracePacket(UT_NODE_TX,0x94,255,data,7);assert(!output_length);
    clock_ms=100;ZbApp_TraceResult(255,0xe9,0);assert(!output_length);
    uint8_t ack[17]={2,3,42,0,0,0,11,0};ZbApp_TraceReply(ack,17,91);
    assert(!output_length);
    unsigned count=nodeUploadHistory.count;ZbApp_TraceReply(ack,16,91);assert(nodeUploadHistory.count==count);
    ZbApp_TraceDump(2);assert(strstr(output,"fragment=20") && strstr(output,"未收到无线接收确认(0xe9)"));
    assert(strstr(output,"窗口校验结果") && strstr(output,"window=12") && strstr(output,"seq=91"));
    assert(strstr(output,"received, not proof of application") && strstr(output,"short=0x714c"));
    size_t before=output_length;ZbApp_TraceDump(2);assert(output_length==before);
    for(unsigned i=0;i<40;i++)ZbApp_TracePacket(UT_NODE_TX,0x94,i,data,7);
    assert(nodeUploadHistory.count==24 && nodeUploadHistory.overwritten>0);
    puts("Node trace PASS: bounded metadata, exact seq/payload fingerprint, per-fragment final status, explicit reply receipt vs application, dump throttling");
}
