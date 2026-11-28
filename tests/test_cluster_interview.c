#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "sensor_clusters.h"
#include "gateway_protocol.h"
#define CHILD_SLOTS 2
#define BASIC_CLUSTER 0
static uint8_t entity;
static uint32_t now=100;
static uint32_t millis(void){return now;}
static bool due(uint32_t n,uint32_t d){return (int32_t)(n-d)>=0;}
static struct {uint8_t used,ieee[8],endpoint,eps[16],epCount,epIndex,stage,retries,hy,modelReady;uint16_t address;uint32_t interviewAt,modelAt;} children[CHILD_SLOTS];
typedef struct {uint16_t dstAddr,nwkAddrOfInterest;} zstack_zdoActiveEndpointReq_t;
typedef struct {uint16_t dstAddr,nwkAddrOfInterest;uint8_t endpoint;} zstack_zdoSimpleDescReq_t;
typedef struct {uint8_t endpoint;uint16_t profileID;uint8_t n_inputClusters,n_outputClusters;uint16_t *pInputClusters,*pOutputClusters;} zstack_SimpleDescriptor_t;
typedef struct {uint16_t srcAddr;uint8_t status;uint16_t nwkAddrOfInterest;uint8_t n_activeEPList,*pActiveEPList;} zstack_zdoActiveEndpointsRspInd_t;
typedef struct {uint16_t srcAddr;uint8_t status;uint16_t nwkAddrOfInterest;zstack_SimpleDescriptor_t simpleDesc;} zstack_zdoSimpleDescRspInd_t;
static unsigned activeRequests,descriptorRequests,zclRequests,lastState,lastEndpoint,lastCluster,lastCommand;
static void emit(uint8_t cmd,const uint8_t *p,uint8_t len){if(cmd==0xA2 && len==12)lastState=p[10];}
static int Zstackapi_ZdoActiveEndpointReq(uint8_t e,zstack_zdoActiveEndpointReq_t *r){(void)e;assert(r->dstAddr==0x1234);activeRequests++;return 0;}
static int Zstackapi_ZdoSimpleDescReq(uint8_t e,zstack_zdoSimpleDescReq_t *r){(void)e;lastEndpoint=r->endpoint;descriptorRequests++;return 0;}
static uint8_t sendZcl(uint16_t a,uint16_t cluster,uint8_t fc,uint8_t cmd,const uint8_t *p,uint8_t len,bool track,int seq)
{(void)fc;(void)p;(void)len;(void)track;(void)seq;assert(a==0x1234);zclRequests++;lastCluster=cluster;lastCommand=cmd;return 0;}
#include "gateway_interview.h"
int main(void)
{
    uint8_t first[]={1,2,3,4,5,6,7,8},second[]={11,12,13,14,15,16,17,18};
    remember(first,0x1234);children[0].endpoint=1;children[0].hy=2;
    remember(second,0x1234);
    assert(childIndex(0x1234)==0 && !memcmp(children[0].ieee,second,8));
    assert(!children[0].endpoint && !children[0].hy);
    remember(second,0x1235);assert(childIndex(0x1234)<0);
    remember(second,0x1234);
    children[0].used=1;children[0].address=0x1234;
    interviewStart(0);assert(lastState==1);
    interviewTick(now);assert(activeRequests==1);
    uint8_t eps[]={7};
    zstack_zdoActiveEndpointsRspInd_t ep={.srcAddr=0x1234,.nwkAddrOfInterest=0x1234,.n_activeEPList=1,.pActiveEPList=eps};
    interviewEndpoints(&ep);interviewTick(now);assert(descriptorRequests==1 && lastEndpoint==7);
    uint16_t clusters[]={0,3,0x402,HY_CONTROL_CLUSTER,HY_DATA_CLUSTER};
    zstack_zdoSimpleDescRspInd_t desc={.srcAddr=0x1234,.nwkAddrOfInterest=0x1234,
        .simpleDesc={.endpoint=7,.profileID=0x104,.n_inputClusters=5,.pInputClusters=clusters}};
    interviewDescriptor(&desc);assert(children[0].endpoint==7 && children[0].stage==3);
    children[0].modelReady=1; /* Simulate the Basic model response. */
    interviewTick(now);assert(lastCluster==HY_CONTROL_CLUSTER && lastCommand==0);
    uint8_t attrs[]={0,0,0,0x21,3,0,1,0,0,0x19,31,0};
    interviewProtocol(0x1234,1,attrs,12);assert(children[0].stage==3);
    interviewProtocol(0x1234,7,attrs,11);assert(children[0].stage==3);
    interviewProtocol(0x1234,7,attrs,12);assert(children[0].stage==6 && lastState!=3);
    interviewTick(now);assert(lastCommand==0 && children[0].stage==6);
    interviewStart(0);assert(children[0].stage==6); /* Duplicate announce doesn't reset handshake. */
    interviewUplink(0,0x99,0);assert(children[0].stage==6);
    interviewUplink(0,0x99,1);assert(children[0].stage==4 && lastState==3);
    children[0].stage=6;interviewUplink(0,CONTROL_STATUS_CMD,7);assert(children[0].stage==6);
    interviewUplink(0,CONTROL_STATUS_CMD,8);assert(children[0].stage==4 && lastState==3);
    uint8_t link[]={1,0,0,0,0,0,4,0},wire[12];
    assert(Gateway_ControlStatusReport(wire,0x1234,link,8,156,-47)==12);
    assert(Gateway_LE16(wire)==0x1234 && !memcmp(wire+2,link,8));
    assert(wire[10]==156 && (int8_t)wire[11]==-47);
    link[0]=2;assert(!Gateway_ControlStatusReport(wire,0x1234,link,8,156,-47));
    assert(hy_cluster_for_command(0x0c)==HY_CONTROL_CLUSTER);
    assert(hy_cluster_for_command(0x99)==HY_CONTROL_CLUSTER);
    assert(hy_cluster_for_command(0x95)==HY_DATA_CLUSTER && hy_cluster_for_command(0x0d)==HY_DATA_CLUSTER);
    interviewStart(0);for(unsigned n=0;n<4;n++){interviewTick(now);now+=4000;}
    assert(children[0].stage==5 && children[0].used && lastState==4);
    interviewStart(0);ep.n_activeEPList=17;interviewEndpoints(&ep);assert(lastState==4);
    interviewStart(0);ep.n_activeEPList=1;interviewEndpoints(&ep);desc.simpleDesc.n_inputClusters=3;
    interviewDescriptor(&desc);assert(children[0].stage==4 && lastState==2 && !children[0].hy);
    puts("Dynamic endpoints, descriptor/protocol validation, HELLO confirmation, timeout retention and retry PASS");
}
