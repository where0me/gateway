/* TI Z-Stack adaptation of the ST gateway wire protocol.
 * All stack requests and UART writes execute in the application task.
 * Interrupt callbacks only wake the task; no keys are printed or reset.
 */
#include "gateway_bridge.h"
#include "gateway_protocol.h"
#include "gateway_exchange_wire.h"
#include "gateway_removal.h"
#include "sensor_clusters.h"
#include "comm_test_protocol.h"
#include "zcomdef.h"
#include "zstackapi.h"
#include "zstackmsg.h"
#include "osal_port.h"
#include "bdb.h"
#include "ti_drivers_config.h"
#include "ti_zstack_config.h"
#include <string.h>
#include "scalar_delivery.h"
#include "control_retry.h"
#include "radio_diag_protocol.h"
#include "window_reply_coalesce.h"
#include "mac_api.h"
#include "zmac.h"
#include <ti/drivers/UART2.h>
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/hal/Hwi.h>

#define GW_EP 8
#define TEMP_CLUSTER 0x0402
#define BASIC_CLUSTER 0x0000
#define RX_SIZE 2048
#define TX_SLOTS 32
#define CHILD_SLOTS 64
#define REMOVE_SLOTS 8
#define ALL_CHANNELS 0x07FFF800UL

static UART2_Handle uart;
static Semaphore_Struct sem;
static Semaphore_Handle wake;
static uint8_t entity;
static uint32_t stackEvents;
static NVINTF_nvFuncts_t *nvApi;
static volatile uint16_t rxRead, rxWrite;
static volatile uint32_t rxDropped;
static uint8_t rxChunk[32], rxRing[RX_SIZE];
static GatewayParser parser;
static uint8_t ready, scanning, source, restoreResult = 0xFF, startupStatus = 0xFE;
static uint8_t sequence, transaction, channelState, migrationChannel, migrationId;
static uint32_t formationRetryAt, channelAt;
static uint8_t permitDuration;
static uint32_t permitDeadline;
static uint8_t permitContinuous;
static uint32_t permitRenewAt;
static bool windowQueryTagged;
static uint8_t channelTarget, channelUpdateId, channelPhase;
static zstack_sysNwkInfoReadRsp_t network;
static struct { uint8_t used, id, cmd, seq;
    uint16_t address; uint32_t at,controlGeneration; int controlChild;
    RadioAckMeta radio; WindowReplyTag windowReply;
    uint32_t admissionToken; bool authorization; uint8_t admissionIeee[8];
} pending[TX_SLOTS];
static ControlRetry controlRetries[CHILD_SLOTS];
static uint8_t controlIdentity[CHILD_SLOTS][8];
static int controlSendingChild=-1;
static struct { uint8_t used, ieee[8],endpoint,eps[16],epCount,epIndex,stage,retries,hy,modelReady;
    uint16_t address; uint32_t interviewAt,modelAt; } children[CHILD_SLOTS];
static struct { uint8_t used, ieee[8]; uint16_t address; uint32_t at; } removing[REMOVE_SLOTS];
static struct { uint8_t active, token, index, count; uint32_t deadline; } snapshot;
static uint16_t outputClusters[] = { BASIC_CLUSTER, TEMP_CLUSTER, 0x0003, HY_CONTROL_CLUSTER, HY_DATA_CLUSTER };
static zstack_SimpleDescriptor_t descriptor = {
    .endpoint = GW_EP, .profileID = 0x0104, .deviceID = 0x0000,
    .deviceVer = 0, .n_outputClusters = 5, .pOutputClusters = outputClusters
};

static uint32_t millis(void) { return (uint32_t)((uint64_t)Clock_getTicks() * Clock_tickPeriod / 1000); }
static bool due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
static bool emitChecked(uint8_t cmd,const uint8_t *data,uint8_t len)
{
    uint8_t frame[259];size_t written=0;
    size_t size=Gateway_Encode(cmd,data,len,frame);
    if(!uart)return false;
    int_fast16_t status=UART2_write(uart,frame,size,&written);
    return status==UART2_STATUS_SUCCESS && written==size;
}
static void emit(uint8_t cmd,const uint8_t *data,uint8_t len)
{
    (void)emitChecked(cmd,data,len);
}
/* Only events with equivalent semantics use the ST diagnostic event IDs. */
static void diagnostic(uint8_t event, const uint8_t *ieee, uint16_t addr,
                       uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint32_t value)
{
    uint8_t p[24] = {1, event};
    uint32_t tick = millis();
    for (unsigned i = 0; i < 4; ++i) { p[2+i] = tick >> (8*i); p[20+i] = value >> (8*i); }
    if (ieee) memcpy(p+6, ieee, 8);
    Gateway_Put16(p+14, addr); p[16]=a; p[17]=b; p[18]=c; p[19]=d;
    emit(0x9F, p, sizeof(p));
}
#include "upload_trace_coordinator.h"
#include "gateway_radio_diag.h"
static void networkInfo(void)
{
    uint8_t p[10];
    if (Zstackapi_sysNwkInfoReadReq(entity, &network) != zstack_ZStatusValues_ZSuccess) {
        ready = 0; startupStatus = 0xFF;
    }
    Gateway_Put16(p, network.panId); p[2]=network.logicalChannel;
    p[3]=ready ? 9 : 0; p[4]=startupStatus; p[5]=source;
    p[6]=restoreResult; p[7]=restoreResult == 2 ? startupStatus : 0;
    p[8]=ready ? 4 : channelState;
    p[9]=GW_CAP_ADMISSION_GRANT;
    emit(0x8A, p, sizeof(p));
}
static bool lookup(uint16_t address, uint8_t ieee[8])
{
    for (unsigned i=0; i<CHILD_SLOTS; ++i)
        if (children[i].used && children[i].address == address) {
            memcpy(ieee, children[i].ieee, 8); return true;
        }
    return false;
}
static uint8_t sendZclForQuery(uint16_t address, uint16_t cluster, uint8_t control,
                               uint8_t cmd, const uint8_t *data, uint8_t len,
                               bool track, int fixedSeq, int querySeq, uint32_t admissionToken)
{
    int slot = -1;
    if (!ready) return 0xFF;
    uint8_t ieee[8];
    bool tagged = querySeq >= 0 && lookup(address,ieee);
    bool authorization = cmd == GW_MONITOR_CONTROL_CMD && len == 1U && data[0] == GW_MONITOR_AUTHORIZE_OP;
    if (authorization) {
        if (!lookup(address,ieee)) return 0xFF;
        for (unsigned i=0; i<TX_SLOTS; ++i)
            if (pending[i].used && pending[i].authorization &&
                !memcmp(pending[i].admissionIeee,ieee,sizeof(ieee)))
                return 0x10; /* The previous AF transaction still owns this send. */
    }
    if (tagged) {
        uint32_t now=millis();
        for (unsigned i=0; i<TX_SLOTS; ++i) {
            if (pending[i].used && pending[i].address==address &&
                window_reply_matches(&pending[i].windowReply,ieee,(uint8_t)querySeq,data,now-pending[i].at)) {
                diagnostic(WINDOW_QUERY_COALESCED_EVENT,ieee,address,(uint8_t)querySeq,
                           pending[i].id,data[6],data[7],radio_u32(data+2));
                return 0;
            }
        }
    }
    uint8_t buffer[258], id, seq = fixedSeq < 0 ? sequence++ : (uint8_t)fixedSeq;
    /* Preserve capacity for waveform grants/ACKs during control bursts. */
    if (cmd != 0x0D) {
        unsigned used=0;
        for(unsigned i=0;i<TX_SLOTS;++i) used += pending[i].used!=0;
        if(used>=TX_SLOTS-4U) return 0x10;
    }
    /* Track every AF transaction until confirm, including untracked commands,
     * so a wrapped transaction ID can never be mistaken for another request. */
    for (unsigned i=0; i<TX_SLOTS; ++i) if (!pending[i].used) { slot=i; break; }
    if (slot < 0) return 0x10;
    do {
        id=transaction++;
        bool collision=false;
        for (unsigned i=0; i<TX_SLOTS; ++i) if (pending[i].used && pending[i].id==id) collision=true;
        if (!collision) break;
    } while (true);
    buffer[0]=control; buffer[1]=seq; buffer[2]=cmd;
    if (len) memcpy(buffer+3, data, len);
    zstack_afDataReq_t req = {0};
    req.dstAddr.addrMode=zstack_AFAddrMode_SHORT;
    req.dstAddr.addr.shortAddr=address; req.dstAddr.endpoint=0;
    for(unsigned i=0;i<CHILD_SLOTS;i++)
        if(children[i].used && children[i].address==address) req.dstAddr.endpoint=children[i].endpoint;
    if(!req.dstAddr.endpoint) return 0xFF;
    req.srcEndpoint=GW_EP; req.clusterID=cluster; req.transID=&id;
    req.radius=30; req.options.ackRequest=true;
    /* NWK encryption is provided by the secured network. ST's normal ZCL
     * requests do not force per-device APS link-key encryption. */
    req.n_payload=(uint16_t)len+3; req.pPayload=buffer;
    const uint8_t submittedId=id; /* AF_DataRequest increments *transID on success. */
    uint32_t submittedAt=millis();
    uint8_t status=(uint8_t)Zstackapi_AfDataReq(entity, &req);
    if (!status) {
        pending[slot].used=track ? 2 : 1; pending[slot].id=submittedId;
        pending[slot].cmd=cmd; pending[slot].seq=seq;
        pending[slot].address=address; pending[slot].at=submittedAt;
        pending[slot].authorization=authorization;
        pending[slot].admissionToken=admissionToken;
        if (authorization) memcpy(pending[slot].admissionIeee,ieee,sizeof(ieee));
        pending[slot].windowReply.valid=tagged;
        if(tagged) {
            pending[slot].windowReply.query_seq=(uint8_t)querySeq;
            memcpy(pending[slot].windowReply.ieee,ieee,sizeof(ieee));
            memcpy(pending[slot].windowReply.payload,data,WINDOW_REPLY_LEN);
        }
        pending[slot].controlChild=controlSendingChild;
        pending[slot].controlGeneration=controlSendingChild>=0 ? controlRetries[controlSendingChild].generation : 0;
        if(cmd==0x0D) {
            pending[slot].radio=radio_ack_meta(data,len);
            radioDiagAf(RADIO_DIAG_SUBMIT,address,submittedId,seq,&pending[slot].radio,status,0);
        }
    }
    return status;
}
static uint8_t sendZcl(uint16_t address, uint16_t cluster, uint8_t control,
                       uint8_t cmd, const uint8_t *data, uint8_t len,
                       bool track, int fixedSeq)
{
    return sendZclForQuery(address,cluster,control,cmd,data,len,track,fixedSeq,-1,0);
}

#include "gateway_interview.h"
static void permitState(void)
{
    uint32_t now=millis();
    uint8_t p[2]={0,0};
    if(permitDuration && !due(now,permitDeadline)) {
        p[0]=1; p[1]=(uint8_t)((permitDeadline-now+999)/1000);
    }
    emit(0xA1,p,sizeof(p));
}
static uint8_t nodeCommandForQuery(uint16_t addr,uint8_t cmd,const uint8_t *p,
                                   uint8_t len,bool track,int querySeq)
{
    bool suppress=cmd==0x0D || cmd==0x10 || cmd==0x11 || cmd==0x12 || cmd==COMM_TEST_REPLY;
    int i=childIndex(addr);
    if(i<0 || children[i].hy!=2) return 0xFF;
    return sendZclForQuery(addr,hy_cluster_for_command(cmd),suppress ? 0x11 : 0x01,
                           cmd,p,len,track,-1,querySeq,0);
}

static uint8_t nodeCommand(uint16_t addr,uint8_t cmd,const uint8_t *p,uint8_t len,bool track)
{
    return nodeCommandForQuery(addr,cmd,p,len,track,-1);
}
static void admissionResult(uint16_t address,uint8_t status,uint8_t seq,uint32_t token,uint8_t stage)
{
    uint8_t p[GW_ADMISSION_RESULT_SIZE];
    Gateway_Put16(p,address);
    p[2]=GW_MONITOR_CONTROL_CMD; p[3]=status; p[4]=seq;
    radio_w32(p+5,token); p[9]=stage;
    emit(0x90,p,sizeof(p));
}

static void controlAttempt(unsigned ci,uint32_t now)
{
    ControlRetry *r=&controlRetries[ci];
    if(memcmp(controlIdentity[ci],children[ci].ieee,8))r->active=0;
    if(!control_retry_due(r,now) || !children[ci].used)return;
    r->attempts++;r->inflight=1;controlSendingChild=(int)ci;
    uint32_t gen=r->generation;
    uint8_t status=nodeCommand(children[ci].address,0x0C,r->payload,r->length,true);
    controlSendingChild=-1;
    uint8_t p[4];Gateway_Put16(p,children[ci].address);p[2]=0x0C;p[3]=status;emit(0x8E,p,4);
    diagnostic(0x37,children[ci].ieee,children[ci].address,0x0C,1,
               r->attempts>255?255:(uint8_t)r->attempts,status,gen);
    if(status) {
        control_retry_result(r,gen,status,now);
        diagnostic(0x37,children[ci].ieee,children[ci].address,0x0C,0,
                   r->attempts>255?255:(uint8_t)r->attempts,status,r->next-now);
    }
}
static void controlApplied(unsigned ci,const uint8_t *p,unsigned n)
{
    if(ci>=CHILD_SLOTS || !children[ci].used ||
       memcmp(controlIdentity[ci],children[ci].ieee,8))return;
    ControlRetry *r=&controlRetries[ci];
    if(control_retry_applied(r,p,n))
        diagnostic(0x37,children[ci].ieee,children[ci].address,0x0C,4,
                   r->attempts>255?255:(uint8_t)r->attempts,0,r->generation);
}
static uint8_t permit(uint8_t duration, uint8_t sourceCmd)
{
    if (!ready) {
        diagnostic(11,NULL,0xFFFF,duration,startupStatus,ready,sourceCmd,0);
        return 0xFF;
    }
    zstack_zdoMgmtPermitJoinReq_t req={0};
    req.nwkAddr=0; req.duration=duration; req.tcSignificance=true;
    uint8_t status=(uint8_t)Zstackapi_ZdoMgmtPermitJoinReq(entity, &req);
    /* TI ZDP submission is not NLME completion; use TI-specific event IDs. */
    diagnostic(0x30,NULL,0,duration,status,sourceCmd,0,0);
    return status;
}
static uint8_t permitWanted(uint8_t open)
{
    permitContinuous=open!=0;permitRenewAt=millis()+5000U;
    return permit(open?254U:0U,open?3U:5U);
}
static void permitTick(uint32_t now)
{
    if(permitContinuous && due(now,permitRenewAt)) {
        permitRenewAt=now+5000U;
        (void)permit(254,0);
    }
}
static void form(void)
{
    zstack_bdbStartCommissioningReq_t req={0};
    req.commissioning_mode=BDB_COMMISSIONING_MODE_NWK_FORMATION;
    channelState=2;
    startupStatus=(uint8_t)Zstackapi_bdbStartCommissioningReq(entity, &req);
    networkInfo();
    if (startupStatus) { channelState=5; formationRetryAt=millis()+5000; }
}
static uint8_t removeDevice(const uint8_t ieee[8], uint16_t address)
{
    int slot=-1;
    if (!ready) return 0xFF;
    uint8_t zero[8]={0};
    if (!memcmp(ieee, zero, 8) || !memcmp(ieee, network.ieeeAddr, 8)) return 2;
    for (unsigned i=0; i<REMOVE_SLOTS; ++i) {
        if (removing[i].used && !memcmp(removing[i].ieee, ieee, 8)) { slot=i; break; }
        if (!removing[i].used && slot<0) slot=i;
    }
    if (slot<0) return 0x10;
    zstack_zdoMgmtLeaveReq_t req={0};
    /* Request local parent to deliver NWK Leave to its child; no direct
     * security/address-table deletion while a sleepy node has not left. */
    req.nwkAddr=0; memcpy(req.deviceAddress, ieee, 8);
    req.options.rejoin=false; req.options.removeChildren=false;
    uint8_t status=(uint8_t)Zstackapi_ZdoMgmtLeaveReq(entity, &req);
    if (!status) { removing[slot].used=1; removing[slot].address=address;
        removing[slot].at=millis(); memcpy(removing[slot].ieee, ieee, 8); }
    diagnostic(0x33,ieee,address,status,0,0,0,0);
    return status;
}
static void removalResults(void)
{
    GatewayRemovalResult r;
    while (GatewayRemoval_Pop(&r)) {
        uint8_t p[14]={0}; memcpy(p,r.ieee,8); Gateway_Put16(p+8,r.address);
        p[10]=r.tc; p[11]=r.app; p[12]=r.nv;
        diagnostic(0x34,r.ieee,r.address,r.evidence,r.status,r.tc,r.nv,0);
        if (r.status) continue; /* Failed Leave is not a DEVICE_LEFT event. */
        bool complete=!r.status && !r.tc && !r.app && !r.nv;
        for (unsigned i=0;i<REMOVE_SLOTS;++i) {
            if (!removing[i].used || memcmp(removing[i].ieee,r.ieee,8)) continue;
            if (complete) { p[13]=1; removing[i].used=0; }
        }
        if (complete) for (unsigned i=0;i<CHILD_SLOTS;++i)
            if (children[i].used && !memcmp(children[i].ieee,r.ieee,8)) {
                children[i].used=0;controlRetries[i].active=0;
            }
        /* Completion is coordinator cleanup + verified NV, not proof that
         * the remote node has finished erasing its own application flash. */
        emit(0x8F,p,sizeof(p));
    }
}
static uint8_t nwkUpdate(uint16_t address, uint8_t channel, uint8_t update)
{
    zstack_zdoMgmtNwkUpdateReq_t req={0};
    req.dstAddr=address; req.channelMask=1UL<<channel;
    req.scanDuration=0xFE; req.nwkUpdateId=update; req.nwkMgrAddr=0;
    return (uint8_t)Zstackapi_ZdoMgmtNwkUpdateReq(entity, &req);
}
static void channelConfig(const GatewayFrame *f)
{
    uint8_t strategy=f->data[0], target=f->data[1], count=f->data[2], phase=f->data[3];
    uint8_t p[7]={strategy,target,2,network.logicalChannel,phase,0,0};
    if (count && f->len>=6) memcpy(p+5,f->data+4,2);
    if (f->len != 4U+2U*count || strategy>1 || (strategy && (target<11 || target>26))) {
        emit(0x96,p,7); return;
    }
    if (!strategy) { channelTarget=0; migrationChannel=0; p[2]=0; }
    else if (!ready) { p[2]=0xFF; }
    else {
        /* Read the stack's current channel/update ID before deciding whether
         * this is a no-op. Do not reserve an ID after a failed read. */
        uint8_t status=(uint8_t)Zstackapi_sysNwkInfoReadReq(entity,&network);
        if (status) { migrationChannel=0; p[2]=status; emit(0x96,p,7); return; }
        p[3]=network.logicalChannel;
        if (network.logicalChannel==target) {
            channelTarget=0; migrationChannel=0; p[2]=0; emit(0x96,p,7); return;
        }
        if (phase==0xFE && !count) { emit(0x96,p,7); return; }
        if (migrationChannel!=target) {
            migrationChannel=target;
            migrationId=network.nwkUpdateId+1; }
        p[2]=0;
        for (unsigned i=0;i<count;++i) {
            uint8_t status=nwkUpdate(Gateway_LE16(f->data+4+2*i),target,migrationId);
            if (status) p[2]=status;
        }
        if (phase!=0xFE && !p[2]) {
            uint8_t status=nwkUpdate(0xFFFD,target,migrationId);
            if (status) p[2]=status;
            else { channelTarget=target; channelUpdateId=migrationId; channelPhase=phase;
                channelAt=millis()+((phase==0xFF || phase==0) ? 1000UL : (uint32_t)phase*1000); }
        }
    }
    /* This reply reports submission, not node migration completion. */
    emit(0x96,p,7);
}
static void channelTick(uint32_t now)
{
    if (!channelTarget || !due(now,channelAt)) return;
    /* Z-Stack does not receive its own broadcast. Submit the matching local
     * update separately; the stack then applies its broadcast-delivery delay.
     * Never turn a rejected local request into silent backend timeout. */
    uint8_t target=channelTarget;
    uint8_t status=nwkUpdate(0,target,channelUpdateId);
    channelTarget=0; migrationChannel=0;
    if (status) {
        uint8_t p[7]={1,target,status,network.logicalChannel,channelPhase,0,0};
        emit(0x96,p,sizeof(p));
    }
    /* This is actual channel readback, not proof that submission completed.
     * The backend keeps polling until it sees the target channel. */
    networkInfo();
}
static void snapshotNext(void)
{
    zstack_zdoMgmtLqiReq_t req={0}; req.nwkAddr=0; req.startIndex=snapshot.index;
    if (Zstackapi_ZdoMgmtLqiReq(entity,&req)) {
        uint8_t p[3]={snapshot.token,snapshot.count,1}; emit(0x9E,p,3); snapshot.active=0;
    }
}
static void command(const GatewayFrame *f)
{
    uint16_t addr=f->len>=2 ? Gateway_LE16(f->data) : 0xFFFF;
    switch (f->cmd) {
    case 1: break; /* Legacy registration must not open the network. */
    case 3: {
        uint8_t status=permitWanted(1);
        /* Actual open state is updated only by DEV_PERMIT_JOIN_IND. */
        if (f->cmd==3) emit(0x83,&status,1);
        break;
    }
    case 5: (void)permitWanted(0); break;
    case 0x13: permitState(); break;
    case 0x14: if(f->len==2) { int i=childIndex(addr); if(i>=0)interviewStart(i); } break;
    case RADIO_DIAG_CMD:
        if(f->len==1 && f->data[0]<=1){radioDiagConfigure(f->data[0]);uploadTraceCapability();}
        else if(f->len==3 && f->data[0]==2U)uploadTraceDump(Gateway_LE16(f->data+1),6);
        else if(f->len==1 && f->data[0]==WINDOW_QUERY_TAG_ENABLE)windowQueryTagged=true;
        break;
    case 2: case 0x11:
        if (f->len==8 || (f->cmd==2 && f->len==2)) {
            uint8_t ieee[8], reply[9]; bool found=f->len==8;
            if (found) memcpy(ieee,f->data,8); else found=lookup(addr,ieee);
            if (found) {
                uint16_t shortAddr=0xFFFF;
                for (unsigned i=0;i<CHILD_SLOTS;++i)
                    if (children[i].used && !memcmp(children[i].ieee,ieee,8)) shortAddr=children[i].address;
                memcpy(reply,ieee,8); reply[8]=removeDevice(ieee,shortAddr);
                if (f->cmd==0x11) emit(0x9C,reply,9);
            }
        }
        break;
    case 4: case 7:
        if (f->len==2) { uint8_t attr[2]={f->cmd==4 ? 5 : 0,0};
            (void)sendZcl(addr,f->cmd==4 ? BASIC_CLUSTER : TEMP_CLUSTER,0x10,0,attr,2,false,-1); }
        break;
    case 8: if (f->len==2) (void)nodeCommand(addr,8,NULL,0,false); break;
    case 0x0A:
        /* A legacy backend only requests network info. A capable backend
         * immediately follows that request with a fresh tag opt-in. */
        windowQueryTagged=false; networkInfo(); break;
    case GX_UART_DOWN:
        if (f->len>=2U) {
            const uint8_t *data=f->data+2;
            unsigned size=f->len-2U;
            if (gx_frame(data,size,GX_RESPONSE,GX_READY,GX_DECISION_SIZE) ||
                gx_frame(data,size,GX_COMMAND,GX_SAMPLING,GX_SAMPLING_SIZE))
                (void)nodeCommand(addr,GX_CONTROL,data,(uint8_t)size,true);
        }
        break;
    case CONTROL_RESUME_SCHEDULE_CMD:
        if (f->len==2U+CONTROL_RESUME_SCHEDULE_SIZE)
            (void)nodeCommand(addr,f->cmd,f->data+2,CONTROL_RESUME_SCHEDULE_SIZE,true);
        break;
    case CONTROL_CONFIG_SYNC_CMD: {
        int ci=childIndex(addr);
        if(f->len==3U+CONTROL_CONFIG_SIZE && f->data[f->len-1]==CONTROL_DELIVERY_WINDOW &&
           ci>=0 && children[ci].hy==2) {
            (void)control_retry_schedule(&controlRetries[ci],f->data+2,CONTROL_CONFIG_SIZE,
                                         CONTROL_DELIVERY_WINDOW,millis());
            memcpy(controlIdentity[ci],children[ci].ieee,8);
            controlRetries[ci].salt=scalar_u32(children[ci].ieee)^scalar_u32(children[ci].ieee+4);
            diagnostic(0x37,children[ci].ieee,addr,0x0C,3,0,0,controlRetries[ci].generation);
            controlAttempt((unsigned)ci,millis());
        } else {uint8_t p[4];Gateway_Put16(p,addr);p[2]=0x0C;p[3]=0xFF;emit(0x8E,p,4);}
        break;
    }
    case GW_CMD_ADMISSION_GRANT:
        if (f->len==6U) {
            uint32_t token=radio_u32(f->data+2);
            if (!token) break;
            int i=childIndex(addr);
            const uint8_t operation=GW_MONITOR_AUTHORIZE_OP;
            uint8_t status=i<0 || children[i].hy!=2 ? 0xFF :
                sendZclForQuery(addr,HY_CONTROL_CLUSTER,0x01,GW_MONITOR_CONTROL_CMD,
                                &operation,1,true,-1,-1,token);
            if (status) admissionResult(addr,status,0xFF,token,GW_ADMISSION_SUBMIT_FAILED);
        }
        break;
    case 0x0B:
        if (f->len>=3) { uint8_t p[4]; Gateway_Put16(p,addr); p[2]=f->cmd;
            p[3]=nodeCommand(addr,f->cmd,f->data+2,f->len-2,true); emit(0x8E,p,4); }
        break;
    case 0x0D: {
        bool tagged=window_reply_has_tag(f->data,f->len);
        if ((f->len==19 || f->len==39 || tagged) && f->data[2]==2) {
            uint8_t length=f->len-2U-(tagged?1U:0U);
            int querySeq=tagged?f->data[f->len-1U]:-1;
            uint8_t status=nodeCommandForQuery(addr,0x0D,f->data+2,length,false,querySeq);
            if(status) {
                RadioAckMeta m=radio_ack_meta(f->data+2,length);
                radioDiagAf(RADIO_DIAG_SUBMIT,addr,0xFF,0xFF,&m,status,0);
            }
        }
        break;
    }
    case 0x10:
        if (f->len==4 && scanning) (void)nodeCommand(addr,0x10,f->data+2,2,false);
        break;
    case 0x0E: if (f->len>=4) channelConfig(f); break;
    case 0x12:
        if (f->len==1 && !snapshot.active) {
            snapshot.active=1; snapshot.token=f->data[0]; snapshot.index=0; snapshot.count=0;
            snapshot.deadline=millis()+5000; snapshotNext();
        }
        break;
    default: break;
    }
}
static void incoming(const zstack_afIncomingMsgInd_t *m)
{
    if (m->endpoint!=GW_EP || !m->srcAddr.endpoint ||
        m->srcAddr.addrMode!=zstack_AFAddrMode_SHORT || !m->pPayload || m->n_payload<3) return;
    const uint8_t *p=m->pPayload; uint16_t size=m->n_payload;
    uint8_t fc=p[0]; unsigned header=(fc&4) ? 5 : 3;
    if (size<header) return;
    /* Manufacturer-specific messages are not part of our sensor protocol. */
    if (fc&4) return;
    uint8_t seq=p[1], cmd=p[2]; p+=header; size-=header;
    uint16_t addr=m->srcAddr.addr.shortAddr; uint8_t out[255]; uint8_t status=0x81;
    Gateway_Put16(out,addr);
    if (m->clusterId==BASIC_CLUSTER && (fc&3)==0 && cmd==1 && size>=4 &&
        Gateway_LE16(p)==5 && p[2]==0 && p[3]==0x42 && size>=5 && size>=5U+p[4]) {
        uint8_t len=p[4]>40 ? 40 : p[4];
        int modelChild=childIndex(addr);
        if(len && modelChild>=0)children[modelChild].modelReady=1;
        out[2]=(uint8_t)m->rssi; out[3]=len;
        memcpy(out+4,p+5,len); emit(0x85,out,4+len); return;
    }
    if(m->clusterId==HY_CONTROL_CLUSTER && (fc&3)==0 && (fc&8) && cmd==1) {
        interviewProtocol(addr,m->srcAddr.endpoint,p,size); return;
    }
    uploadTraceIncoming(UT_COORD_RX,addr,cmd,seq,p,size,0,m->rssi,m->linkQuality);
    int ci=childIndex(addr);
    if(ci<0 || children[ci].hy!=2 || children[ci].endpoint!=m->srcAddr.endpoint ||
       m->clusterId!=hy_cluster_for_command(cmd) || (fc&3)!=1 || !(fc&8)) {
        uploadTraceIncoming(UT_COORD_DROP,addr,cmd,seq,p,size,ci<0?1:children[ci].hy!=2?2:3,m->rssi,m->linkQuality);
        return;
    }
    if (cmd==CONTROL_STATUS_CMD &&
        !control_status_valid(p,size) && !control_final_status_valid(p,size)) return;
    if (cmd==SCALAR_MANIFEST_CMD && !scalar_manifest_valid(p,size)) {
        uploadTraceIncoming(UT_COORD_DROP,addr,cmd,seq,p,size,4,m->rssi,m->linkQuality);return;
    }
    if(cmd!=COMM_TEST_REQUEST)interviewUplink((unsigned)ci,cmd,size);
    if (cmd==GX_DATA &&
        (gx_frame(p,size,GX_EVENT,GX_READY,GX_READY_SIZE) ||
         gx_frame(p,size,GX_EVENT,GX_CONTEXT,GX_CONTEXT_SIZE) ||
         gx_frame(p,size,GX_EVENT,GX_CONTEXT,GX_CONTEXT_SIZE+GX_SCALAR_SIZE))) {
        memcpy(out+2,p,size);
        emit(GX_UART_UP,out,(uint8_t)(size+2U));
        status=0;
    } else if (cmd==GX_CONTROL && gx_frame(p,size,GX_RESPONSE,GX_SAMPLING,GX_SAMPLING_RESULT_SIZE)) {
        memcpy(out+2,p,size);
        emit(GX_UART_UP,out,(uint8_t)(size+2U));
        status=0;
    } else if(cmd==COMM_TEST_REQUEST && size==COMM_TEST_REQUEST_LEN && p[0]==COMM_TEST_VERSION) {
        uint8_t echo[COMM_TEST_REPLY_LEN],report[COMM_TEST_REPORT_LEN];
        memcpy(echo,p,COMM_TEST_REQUEST_LEN);echo[5]=(uint8_t)m->rssi;echo[6]=m->linkQuality;
        uint8_t submitted=nodeCommand(addr,COMM_TEST_REPLY,echo,sizeof(echo),false);
        Gateway_Put16(report,addr);memcpy(report+2,echo,sizeof(echo));report[9]=submitted;
        emit(COMM_TEST_REPORT,report,sizeof(report));status=0;
    } else if (cmd==0x9C && size==10) {
        uint8_t ieee[8];
        if (lookup(addr,ieee)) { memcpy(out,ieee,8); Gateway_Put16(out+8,addr); emit(0xA0,out,10); }
        (void)nodeCommand(addr,0x11,p,10,false); status=0;
    } else if (cmd==CONTROL_STATUS_CMD) {
        if (size==CONTROL_STATUS_SIZE || (p[1]&CONTROL_APPLIED))
            controlApplied((unsigned)ci,p,CONTROL_STATUS_SIZE);
        uint8_t length=Gateway_ControlStatusReport(out,addr,p,size,m->linkQuality,m->rssi);
        if (length) { emit(cmd,out,length); status=0; }
    } else if (cmd==0x8D && (size==6 || size==7)) {
        uint8_t length=Gateway_MonitorReport(out,addr,p,size,m->linkQuality,m->rssi);
        bool sent=emitChecked(cmd,out,length);
        uploadTraceIncoming(UT_COORD_FORWARD,addr,cmd,seq,p,size,sent?0:4,m->rssi,m->linkQuality); status=0;
    } else if (cmd==0x9B && size==2 && scanning) {
        /* Preserve the old RSSI offset; append LQI from this exact reply. */
        memcpy(out+2,p,2); out[4]=(uint8_t)m->rssi; out[5]=m->linkQuality;
        emit(cmd,out,6); status=0;
    } else if ((cmd>=0x86 && cmd<=0x89) || cmd==0x8B || cmd==0x8C || cmd==SCALAR_MANIFEST_CMD ||
               (cmd>=0x92 && cmd<=0x95) || cmd==0x99) {
        if (size<=253) {
            memcpy(out+2,p,size);
            uint8_t wireSize=(uint8_t)(size+2);
            if (windowQueryTagged && window_query_is_taggable(cmd,p,size)) out[wireSize++]=seq;
            bool sent=emitChecked(cmd,out,wireSize);
            uploadTraceIncoming(UT_COORD_FORWARD,addr,cmd,seq,p,size,sent?0:4,m->rssi,m->linkQuality);
            status=0;
        }
        else status=0x89;
    }
    if (!(fc&0x10)) { uint8_t response[2]={cmd,status};
        (void)sendZcl(addr,m->clusterId,0x10,0x0B,response,2,false,seq); }
}
static void becameReady(void)
{
    if (ready) return;
    ready=1; startupStatus=0; channelState=4; formationRetryAt=0;
    if (source==0) { source=1; restoreResult=1; }
    (void)permit(0,0); /* Startup restores network but never implicitly opens join. */
    networkInfo();
    uint8_t p[5]={restoreResult,0,0,0,network.logicalChannel};
    Gateway_Put16(p+2,network.panId); emit(0x9A,p,5);
    Gateway_Put16(p,network.panId); p[2]=network.logicalChannel; p[3]=0; p[4]=source;
    emit(0x98,p,5);
}
static void stackMessage(zstackmsg_genericReq_t *msg)
{
    switch (msg->hdr.event) {
    case zstackmsg_CmdIDs_DEV_PERMIT_JOIN_IND: {
        uint8_t duration=((zstackmsg_devPermitJoinInd_t *)msg)->Req.duration;
        permitDuration=duration; permitDeadline=millis()+(uint32_t)duration*1000;
        if(permitContinuous)permitRenewAt=millis()+(duration>30U?(uint32_t)(duration-30U)*1000U:1000U);
        scanning=duration!=0; permitState();
        diagnostic(0x31,NULL,0,duration,0,0,0,0);
        break;
    }
    case zstackmsg_CmdIDs_ZDO_MGMT_PERMIT_JOIN_RSP: {
        zstack_zdoMgmtPermitJoinRspInd_t *r=&((zstackmsg_zdoMgmtPermitJoinRspInd_t *)msg)->rsp;
        diagnostic(0x32,NULL,r->srcAddr,(uint8_t)r->status,0,0,0,0);
        break;
    }
    case zstackmsg_CmdIDs_DEV_STATE_CHANGE_IND: {
        zstackmsg_devStateChangeInd_t *m=(void *)msg;
        if (m->req.state==zstack_DevState_DEV_ZB_COORD) becameReady();
        break;
    }
    case zstackmsg_CmdIDs_BDB_NOTIFICATION: {
        bdbCommissioningModeMsg_t *b=&((zstackmsg_bdbNotificationInd_t *)msg)->Req;
        if (b->bdbCommissioningMode==BDB_COMMISSIONING_INITIALIZATION && !ready) {
            zstack_sysNwkInfoReadRsp_t info={0};
            if (!Zstackapi_sysNwkInfoReadReq(entity,&info) && info.devState==zstack_DevState_DEV_ZB_COORD) becameReady();
            else { source=2; restoreResult=0; form(); }
        } else if (b->bdbCommissioningMode==BDB_COMMISSIONING_FORMATION) {
            if (!b->bdbCommissioningStatus) becameReady();
            else { startupStatus=b->bdbCommissioningStatus; channelState=5;
                formationRetryAt=millis()+5000; networkInfo(); }
        }
        break;
    }
    case zstackmsg_CmdIDs_ZDO_DEVICE_ANNOUNCE: {
        zstack_zdoDeviceAnnounceInd_t *a=&((zstackmsg_zdoDeviceAnnounceInd_t *)msg)->req;
        uint8_t p[10]; memcpy(p,a->devExtAddr,8); Gateway_Put16(p+8,a->devAddr);
        remember(a->devExtAddr,a->devAddr); emit(0x84,p,10);
        diagnostic(7,a->devExtAddr,a->devAddr,0,0,0,scanning,0);
        int ci=childIndex(a->devAddr); if(ci>=0)interviewStart(ci); break;
    }
    case zstackmsg_CmdIDs_ZDO_ACTIVE_EP_RSP:
        interviewEndpoints(&((zstackmsg_zdoActiveEndpointsRspInd_t *)msg)->rsp); break;
    case zstackmsg_CmdIDs_ZDO_SIMPLE_DESC_RSP:
        interviewDescriptor(&((zstackmsg_zdoSimpleDescRspInd_t *)msg)->rsp); break;
    case zstackmsg_CmdIDs_ZDO_LEAVE_IND: {
        zstack_zdoLeaveInd_t *l=&((zstackmsg_zdoLeaveInd_t *)msg)->req;
        uint8_t p[14]={0}; memcpy(p,l->extendedAddr,8); Gateway_Put16(p+8,l->srcAddr);
        diagnostic(8,l->extendedAddr,l->srcAddr,l->rejoin,0,0,scanning,0);
        /* Cleanup result is produced in the stack task, covering both
         * successful child LeaveCnf and remote LeaveInd paths. */
        emit(0x8F,p,10);
        break;
    }
    case zstackmsg_CmdIDs_AF_INCOMING_MSG_IND:
        incoming(&((zstackmsg_afIncomingMsgInd_t *)msg)->req); break;
    case zstackmsg_CmdIDs_AF_DATA_CONFIRM_IND: {
        zstack_afDataConfirmInd_t *c=&((zstackmsg_afDataConfirmInd_t *)msg)->req;
        if (c->endpoint!=GW_EP) break;
        for (unsigned i=0;i<TX_SLOTS;++i) if (pending[i].used && pending[i].id==c->transID) {
            if(pending[i].cmd==0x0D) {
                radioDiagAf(RADIO_DIAG_AF_RESULT,pending[i].address,
                    pending[i].id,pending[i].seq,&pending[i].radio,(uint8_t)c->status,millis()-pending[i].at);
            }
            if (pending[i].admissionToken)
                admissionResult(pending[i].address,(uint8_t)c->status,pending[i].seq,
                                pending[i].admissionToken,GW_ADMISSION_AF_RESULT);
            else if (pending[i].used==2) { uint8_t p[5]; Gateway_Put16(p,pending[i].address);
                p[2]=pending[i].cmd; p[3]=(uint8_t)c->status; p[4]=pending[i].seq; emit(0x90,p,5); }
            if(pending[i].controlChild>=0 && pending[i].cmd==0x0C) {
                unsigned ci=(unsigned)pending[i].controlChild;ControlRetry *r=&controlRetries[ci];
                int current=r->generation==pending[i].controlGeneration;
                control_retry_result(r,pending[i].controlGeneration,(uint8_t)c->status,millis());
                if(current)diagnostic(0x37,children[ci].ieee,children[ci].address,0x0C,
                    r->applied?5:c->status?0:2,
                    r->attempts>255?255:(uint8_t)r->attempts,(uint8_t)c->status,
                    !r->applied && c->status?r->next-millis():r->generation);
            }
            pending[i].used=0; break;
        }
        break;
    }
    case zstackmsg_CmdIDs_ZDO_MGMT_LQI_RSP: {
        zstack_zdoMgmtLqiRspInd_t *r=&((zstackmsg_zdoMgmtLqiRspInd_t *)msg)->rsp;
        if (!snapshot.active || r->srcAddr!=0 || r->startIndex!=snapshot.index) break;
        if (r->status) { uint8_t p[3]={snapshot.token,snapshot.count,1}; emit(0x9E,p,3); snapshot.active=0; break; }
        for (unsigned i=0;i<r->n_lqiList;++i) {
            zstack_nwkLqiItem_t *n=&r->pLqiList[i];
            if (n->relationship!=zstack_RelationTypes_CHILD) continue;
            uint8_t p[14]={snapshot.token}; memcpy(p+1,n->extendedAddr,8); Gateway_Put16(p+9,n->nwkAddr);
            p[11]=n->rxLqi; p[12]=0x80; p[13]=1; /* ST CHILD value; RSSI unavailable in Mgmt_Lqi. */
            remember(n->extendedAddr,n->nwkAddr); emit(0x9D,p,14); ++snapshot.count;
        }
        unsigned next=(unsigned)r->startIndex+r->n_lqiList;
        if (r->n_lqiList && next<r->neighborLqiEntries && next<64) {
            snapshot.index=next; snapshotNext();
        } else { uint8_t p[3]={snapshot.token,snapshot.count,next<r->neighborLqiEntries ? 2 : 0};
            emit(0x9E,p,3); snapshot.active=0; }
        break;
    }
    default: break;
    }
}
static void readDone(UART2_Handle handle, void *buffer, size_t count, void *user, int_fast16_t status)
{
    (void)user;
    if (status==UART2_STATUS_SUCCESS) {
        const uint8_t *bytes=(const uint8_t *)buffer;
        for(size_t i=0;i<count;++i) {
            uint16_t next=(rxWrite+1)%RX_SIZE;
            if (next!=rxRead) { rxRing[rxWrite]=bytes[i]; rxWrite=next; } else ++rxDropped;
        }
    } else {
        ++rxDropped; /* Reset framing instead of accepting a truncated command. */
    }
    Semaphore_post(wake);
    (void)UART2_read(handle,rxChunk,sizeof(rxChunk),NULL);
}
void GatewayBridge_Run(NVINTF_nvFuncts_t *nv)
{
    nvApi=nv; (void)nvApi; /* Network persistence remains stack-owned NV_RESTORE. */
    Semaphore_Params sp; Semaphore_Params_init(&sp); sp.mode=Semaphore_Mode_BINARY;
    Semaphore_construct(&sem,0,&sp); wake=Semaphore_handle(&sem);
    entity=OsalPort_registerTask(Task_self(),wake,&stackEvents);
    UART2_Params up; UART2_Params_init(&up); up.baudRate=921600;
    up.readMode=UART2_Mode_CALLBACK; up.writeMode=UART2_Mode_BLOCKING; up.readCallback=readDone;
    /* Partial mode flushes short commands on hardware RX inactivity too.
     * It does not wait for all 32 bytes and avoids one callback per byte. */
    up.readReturnMode=UART2_ReadReturnMode_PARTIAL;
    uart=UART2_open(CONFIG_GATEWAY_UART,&up);
    if (!uart) { for (;;) Task_sleep(1000); }
    (void)UART2_read(uart,rxChunk,sizeof(rxChunk),NULL);
    zstack_afRegisterReq_t endpoint={0}; endpoint.endpoint=GW_EP; endpoint.pSimpleDesc=&descriptor;
    if (Zstackapi_AfRegisterReq(entity,&endpoint)) { startupStatus=0xFF; channelState=5; networkInfo(); for (;;) Task_sleep(1000); }
    zstack_devZDOCBReq_t cb={0};
    cb.has_devStateChange=cb.devStateChange=true;
    cb.has_deviceAnnounce=cb.deviceAnnounce=true;
    cb.has_leaveIndCB=cb.leaveIndCB=true;
    cb.has_mgmtLqiRsp=cb.mgmtLqiRsp=true;
    cb.has_activeEndpointRsp=cb.activeEndpointRsp=true;
    cb.has_simpleDescRsp=cb.simpleDescRsp=true;
    cb.has_devPermitJoinInd=cb.devPermitJoinInd=true;
    cb.has_mgmtPermitJoinRsp=cb.mgmtPermitJoinRsp=true;
    (void)Zstackapi_DevZDOCBReq(entity,&cb);
    zstack_bdbSetAttributesReq_t attrs={0};
    attrs.has_bdbPrimaryChannelSet=attrs.has_bdbSecondaryChannelSet=true;
    attrs.bdbPrimaryChannelSet=ALL_CHANNELS; attrs.bdbSecondaryChannelSet=0;
    (void)Zstackapi_bdbSetAttributesReq(entity,&attrs);
    /* Start initialization only: the stack restores NV if present. Formation
     * is requested afterwards only if there is no restored coordinator. */
    channelState=1; networkInfo();
    zstack_bdbStartCommissioningReq_t boot={0};
    boot.commissioning_mode=BDB_COMMISSIONING_MODE_INITIALIZATION;
    startupStatus=(uint8_t)Zstackapi_bdbStartCommissioningReq(entity,&boot);
    if (startupStatus) { channelState=5; networkInfo(); }
    for (;;) {
        (void)Semaphore_pend(wake,10000); /* 100 ms with this project's 10 us tick. */
        radioDiagDrain();
        uint32_t dropped; UInt key=Hwi_disable(); dropped=rxDropped; rxDropped=0;
        if (dropped) rxRead=rxWrite; /* Discard the truncated command stream. */
        Hwi_restore(key);
        if (dropped) { memset(&parser,0,sizeof(parser)); diagnostic(0xFF,NULL,0xFFFF,0,0,0,0,dropped); }
        for (unsigned i=0;i<RX_SIZE && rxRead!=rxWrite;++i) {
            uint8_t b=rxRing[rxRead]; rxRead=(rxRead+1)%RX_SIZE;
            if (Gateway_ParseByte(&parser,b)) command(&parser.frame);
        }
        /* UART commands run first in the application task, never in the ISR.
         * APS submission does not bypass the parent's sleepy-child queue. */
        zstackmsg_genericReq_t *msg;
        unsigned processed=0;
        while(processed<16 && (msg=(void *)OsalPort_msgReceive(entity))!=NULL) {
            stackMessage(msg);++processed;
            if (!Zstackapi_freeIndMsg(msg)) OsalPort_msgDeallocate((uint8_t *)msg);
        }
        if(processed==16 || rxRead!=rxWrite)Semaphore_post(wake);
        uint32_t now=millis();
        removalResults();
        permitTick(now);
        if(scanning && due(now,permitDeadline)) { scanning=permitDuration=0; permitState(); }
        interviewTick(now);
        for(unsigned ci=0;ci<CHILD_SLOTS;ci++)controlAttempt(ci,now);
        if (!ready && formationRetryAt && due(now,formationRetryAt)) { formationRetryAt=0; form(); }
        channelTick(now);
        if (snapshot.active && due(now,snapshot.deadline)) { uint8_t p[3]={snapshot.token,snapshot.count,1}; emit(0x9E,p,3); snapshot.active=0; }
        /* Do not recycle AF IDs after an arbitrary timeout: late confirms are
         * indistinguishable after reuse. A stuck stack stops submission safely. */
        for (unsigned i=0;i<REMOVE_SLOTS;++i)
            if (removing[i].used==1 && due(now,removing[i].at+30000)) {
                diagnostic(0x35,removing[i].ieee,removing[i].address,0,0,0,0,30000);
                /* Retain correlation for a late confirmation. A retry for
                 * this IEEE reuses the slot; never turn timeout into success. */
                removing[i].used=2;
            }
    }
}
