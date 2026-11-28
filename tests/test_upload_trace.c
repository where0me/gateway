#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "upload_trace_format.h"
static uint32_t clock_ms;
static uint32_t millis(void){return clock_ms;}
static uint8_t emitted[128][UT_LEN];static unsigned emitted_count;
static void emit(uint8_t cmd,const uint8_t *p,uint8_t n){
    assert(cmd==0x9F && n==UT_LEN && emitted_count<128);
    memcpy(emitted[emitted_count++],p,n);
}
#include "upload_trace_coordinator.h"
int main(void)
{
    uint8_t data[79]={0};ut_w32(data,0x81234567);ut_w16(data+4,11);data[6]=19;
    for(unsigned i=7;i<79;i++)data[i]=(uint8_t)i;
    UploadTraceRecord a,b;
    assert(ut_packet(&a,UT_COORD_RX,100,0x714C,0x94,255,data,sizeof(data)));
    assert(ut_packet(&b,UT_HOST_RX,90000,0x714C,0x94,-1,data,sizeof(data)));
    assert(ut_u32(a.p+20)==ut_u32(b.p+20));
    assert(ut_u32(a.p+12)==0x81234567 && ut_u16(a.p+16)==11 && a.p[18]==19);
    char text[1400];assert(ut_format(a.p,40,text,sizeof(text)));
    assert(strstr(text,"窗口=12") && strstr(text,"分片=20") && strstr(text,"ZCL序号=255"));
    assert(ut_format(b.p,40,text,sizeof(text)) && strstr(text,"ZCL序号=未提供"));
    data[78]^=1;assert(ut_packet(&b,UT_HOST_RX,0,0x714C,0x94,-1,data,79));
    assert(ut_u32(a.p+20)!=ut_u32(b.p+20));data[78]^=1;
    /* Truncated data is never read past end; no body enters metadata. */
    for(unsigned n=0;n<7;n++)assert(!ut_packet(&b,1,0,0x714C,0x94,0,data,n));
    assert(!ut_packet(&b,1,0,0,0x94,0,NULL,0));
    assert(!ut_packet(&b,1,0,0,0x94,0,data,256));
    uint8_t scalar[10]={0x78,0x56,0x34,0x12};
    assert(ut_packet(&b,1,0,0x714C,0x87,0,scalar,10) && ut_u32(b.p+12)==0x12345678);
    uint8_t monitor[9]={1,5,0x78,0x56,0x34,0x12,2,130,200};
    assert(ut_packet(&a,1,0,0x714C,0x8D,0,monitor,7));
    assert(ut_packet(&b,4,0,0x714C,0x8D,-1,monitor,6));
    assert(ut_u32(a.p+20)==ut_u32(b.p+20));
    for(unsigned i=0;i<140;i++){
        clock_ms=i;
        uploadTraceIncoming(UT_COORD_RX,0x714C,0x94,(uint8_t)i,data,79,0,-47,150);
        uploadTraceIncoming(UT_COORD_FORWARD,0x714C,0x94,(uint8_t)i,data,79,0,-47,150);
    }
    assert(!emitted_count && uploadTraceHistory.count==96 && uploadTraceHistory.overwritten==184);
    clock_ms=200;uploadTraceDump(0x714C,6);assert(emitted_count==50);
    assert(emitted[0][1]==UT_SNAPSHOT && emitted[49][1]==UT_SNAPSHOT_END);
    for(unsigned i=1;i<49;i++){
        assert(ut_u16(emitted[i]+6)==0x714C && (emitted[i][19]&UT_SNAPSHOT_FLAG));
        if(i>1)assert(ut_u32(emitted[i]+32)>ut_u32(emitted[i-1]+32));
    }
    uploadTraceDump(0x714C,1);assert(emitted_count==50);
    /* A separate device is not silenced by the noisy node's rate limit. */
    uploadTraceDump(0x2222,6);assert(emitted_count==52);
    clock_ms+=10000;emitted_count=0;
    uint8_t ack[17]={2,3};ut_w32(ack+2,0x81234567);ut_w16(ack+6,11);ack[8]=1;
    ut_w32(ack+9,1U<<19);
    RadioAckMeta meta=radio_ack_meta(ack,17);
    assert(meta.fingerprint==ut_fingerprint(ack,17));
    uploadTraceAf(RADIO_DIAG_AF_RESULT,0x714C,255,255,&meta,0xF0,6707);
    assert(emitted_count==50);
    assert(ut_format(emitted[48],40,text,sizeof(text)));
    assert(strstr(text,"窗口校验结果") && strstr(text,"AF事务=255") && strstr(text,"6707 ms") && strstr(text,"过期或被清理"));
    assert(ut_u32(emitted[48]+20)==meta.fingerprint);
    assert(!ut_valid(emitted[0],39));emitted[0][1]=99;assert(!ut_valid(emitted[0],40));
    puts("Upload trace PASS: matching payload fingerprints/metadata, seq255, byte mutation, malformed frames, ring wrap, per-node cooldown, bounded chronological snapshots, exact reply result identity");
}
