/* The runner appends the actual bridge channel functions, not a copied model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t data[255]; unsigned len; } GatewayFrame;
typedef struct {
    uint16_t dstAddr, nwkMgrAddr;
    uint32_t channelMask;
    uint8_t scanDuration, nwkUpdateId;
} zstack_zdoMgmtNwkUpdateReq_t;
typedef struct { uint8_t logicalChannel, nwkUpdateId; } Network;
static Network network, stackNetwork;
static uint8_t entity, ready, migrationChannel, migrationId;
static uint8_t channelTarget, channelUpdateId, channelPhase;
static uint32_t channelAt, nowTick;
static uint8_t readStatus, broadcastStatus, localStatus, reply[7];
static unsigned replies, requests, readbacks;
static zstack_zdoMgmtNwkUpdateReq_t sent[4];
static uint32_t millis(void) { return nowTick; }
static bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now-deadline)>=0; }
static uint16_t Gateway_LE16(const uint8_t *p) { return p[0]|((uint16_t)p[1]<<8); }
static void emit(uint8_t cmd,const uint8_t *p,uint8_t len)
{ assert(cmd==0x96 && len==7); memcpy(reply,p,7); ++replies; }
static void networkInfo(void) { ++readbacks; }
static uint8_t Zstackapi_sysNwkInfoReadReq(uint8_t task,Network *out)
{ (void)task; if (!readStatus) *out=stackNetwork; return readStatus; }
static uint8_t Zstackapi_ZdoMgmtNwkUpdateReq(uint8_t task,zstack_zdoMgmtNwkUpdateReq_t *req)
{
    (void)task; assert(requests<4); sent[requests++]=*req;
    assert(req->scanDuration==0xFE && req->nwkMgrAddr==0);
    return req->dstAddr==0 ? localStatus : broadcastStatus;
}
static void channelConfig(const GatewayFrame *f);
static void channelTick(uint32_t now);
static GatewayFrame command(uint8_t phase,uint8_t count)
{
    GatewayFrame f={{1,20,count,phase,0x34,0x12},4U+2U*count}; return f;
}
static void reset(void)
{
    network=(Network){15,7}; stackNetwork=network; ready=1;
    migrationChannel=migrationId=channelTarget=channelUpdateId=channelPhase=0;
    channelAt=nowTick=0; readStatus=broadcastStatus=localStatus=0;
    replies=requests=readbacks=0; memset(reply,0,sizeof(reply));
}
int main(void)
{
    GatewayFrame f=command(1,0);
    reset(); channelConfig(&f);
    assert(reply[2]==0 && requests==1 && sent[0].dstAddr==0xFFFD);
    assert(sent[0].channelMask==(1UL<<20) && sent[0].nwkUpdateId==8);
    assert(channelTarget==20 && channelAt==1000);
    channelTick(999); assert(requests==1);
    channelTick(1000); assert(requests==2 && sent[1].dstAddr==0 && sent[1].nwkUpdateId==8);
    assert(!channelTarget && !migrationChannel && replies==1 && readbacks==1);
    channelTick(2000); assert(requests==2); /* no repeated local submission */

    reset(); broadcastStatus=0x12; channelConfig(&f);
    assert(reply[2]==0x12 && !channelTarget);
    channelTick(2000); assert(requests==1 && readbacks==0);

    reset(); localStatus=0x11; channelConfig(&f); channelTick(1000);
    assert(replies==2 && reply[0]==1 && reply[1]==20 && reply[2]==0x11);
    assert(reply[3]==15 && reply[4]==1 && !channelTarget && readbacks==1);

    reset(); f=command(0xFE,1); channelConfig(&f);
    assert(requests==1 && sent[0].dstAddr==0x1234 && !channelTarget && migrationId==8);
    f=command(0xFF,0); channelConfig(&f); assert(channelTarget==20 && channelAt==1000);
    localStatus=0x10; channelTick(1000); assert(reply[2]==0x10 && reply[4]==0xFF);

    reset(); f=command(1,0); readStatus=0x10; channelConfig(&f);
    assert(reply[2]==0x10 && requests==0 && !migrationChannel);
    readStatus=0; stackNetwork.nwkUpdateId=9; channelConfig(&f);
    assert(sent[0].nwkUpdateId==10); /* fresh ID after failed read */

    reset(); stackNetwork.logicalChannel=20; channelConfig(&f);
    assert(reply[2]==0 && reply[3]==20 && requests==0);

    reset(); f=command(0xFE,0); channelConfig(&f);
    assert(reply[2]==2 && requests==0 && !channelTarget);

    reset(); f=command(1,0); f.data[1]=27; channelConfig(&f);
    assert(reply[2]==2 && requests==0);
    f=command(1,0); ready=0; channelConfig(&f); assert(reply[2]==0xFF && requests==0);

    reset(); f=command(1,0); channelConfig(&f); f.data[0]=0; channelConfig(&f);
    channelTick(2000); assert(!channelTarget && requests==1 && reply[2]==0);

    reset(); f=command(1,0); stackNetwork.nwkUpdateId=255; nowTick=UINT32_MAX-499;
    channelConfig(&f); assert(sent[0].nwkUpdateId==0 && channelAt==500);
    channelTick(499); assert(requests==1); channelTick(500); assert(requests==2);
    puts("TI channel: 10 mocked channel scenarios passed (actual bridge functions)");
    return 0;
}
