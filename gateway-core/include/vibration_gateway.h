/* Included once by main.c, after database/UART/WebSocket helpers.
 * Protected by g_vibration_mutex, including removal. No raw data in coordinator RAM. */
#define V2_GATEWAY_SLOTS 16
#include "upload_admission.h"
#ifndef GATEWAY_WS_EVENT_READY
#define GATEWAY_WS_EVENT_READY() (g_has_new_ws_msg=1)
#endif
#include "report_dedup.h"
#include "scalar_db.h"
#include "upload_lease.h"
#include "upload_completion.h"
#include "vibration_wire.h"
_Static_assert(UPLOAD_QUEUE_IDLE_MS>=UPLOAD_LEASE_MS+30000U,"Server must outlive node upload lease with drain margin");
#ifndef UPLOAD_RESTART_GUARD_MS
#define UPLOAD_RESTART_GUARD_MS 0U
#endif
static uint64_t v2_gateway_boot_ms;
static int v2_gateway_boot_known;
#ifndef GATEWAY_MQTT_BATCH_EVENT
#define GATEWAY_MQTT_BATCH_EVENT(mac,batch,raw) ((void)0)
#endif
#ifndef GATEWAY_SCALAR_POLICY_EVENT
#define GATEWAY_SCALAR_POLICY_EVENT(mac,batch,required,unavailable) (0)
#endif
#ifndef GATEWAY_SCALAR_UNAVAILABLE_EVENT
#define GATEWAY_SCALAR_UNAVAILABLE_EVENT(mac,address,batch,unavailable) ((void)0)
#endif
static UploadAdmission v2_upload_admission;
static ReportDedup v2_queue_reports;
typedef struct {
    V2Rx rx;
    char mac[24];
    uint32_t batch_id;
    uint16_t short_addr;
    uint64_t last_fragment_ms, last_reply_ms;
    int fragment_pending, last_fragment_seen;
    time_t touched;
    uint64_t first_block_started_ms;
    int first_block_timing_active;
    int first_block_receive_logged;
    int first_block_had_missing;
    uint8_t scalar_manifest[SCALAR_MANIFEST_LEN];
    uint8_t scalar_started,scalar_ready,scalar_unavailable_logged;
} V2GatewaySlot;
static V2GatewaySlot v2_gateway_slots[V2_GATEWAY_SLOTS];

static uint64_t v2_gateway_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void v2_gateway_wall_time(char text[32])
{
    struct timespec now;
    struct tm local;
    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &local);
    snprintf(text, 32, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec,
             now.tv_nsec / 1000000L);
}

static int v2_gateway_stored(void *context,const uint8_t *descriptor)
{
    return vibration_batch_matches((const char *)context,descriptor);
}
static int v2_gateway_store(void *context,const uint8_t *descriptor,const uint8_t *raw)
{
    const char *mac=(const char *)context;
    uint32_t batch=v2_u32(descriptor+1),count=v2_u32(descriptor+7);
    int32_t features[3][5];
    for (unsigned i=0;i<15;++i) features[i/5][i%5]=(int32_t)v2_u32(descriptor+17+i*4);
    if (!is_device_in_db(mac) || save_vibration_batch(mac,batch,v2_u16(descriptor+5),
          count,raw,raw?(size_t)count*6:0,features)!=0) return 0;
    if (raw)
        printf("✅ [振动上传] MAC=%s | 批次=%u | %u点已完整校验并入库 | CRC32=%08X\n",
               mac,batch,count,v2_u32(descriptor+13));
    else
        printf("▶ [上传许可] MAC=%s | 批次=%u | 振动点数=%u | 批次申请已接受，等待节点发送数据\n",
               mac,batch,count);
    publish_collection_data_ready_event(mac,batch,raw!=NULL);
    return 1;
}
static void v2_gateway_progress(void *context,uint32_t batch,unsigned blocks,unsigned total)
{
    const char *mac=(const char *)context;
    /* Called only after a newly received window passed decoding and CRC. */
    upload_raw_seen(&v2_upload_admission,mac,batch);
    upload_progress(&v2_upload_admission,mac,batch,blocks,v2_gateway_monotonic_ms());
    printf("✅ [振动窗口] MAC=%s | 批次=%u | 已校验 %u/%u\n",mac,batch,blocks,total);
    pthread_mutex_lock(&g_ws_mutex);
    snprintf(g_ws_msg_buffer,sizeof(g_ws_msg_buffer),
             "{\"type\":\"vibration_upload_progress\",\"mac\":\"%s\",\"batch_id\":%u,"
             "\"received_packets\":%u,\"total_packets\":%u,\"unit\":\"blocks\",\"status\":\"uploading\"}",
             mac,batch,blocks,total);
    GATEWAY_WS_EVENT_READY();
    pthread_mutex_unlock(&g_ws_mutex);
}
/* A BATCH request is made after capture. WAIT is a real lifecycle transition,
 * even though no waveform descriptor/fragment has been admitted yet. */
static void v2_gateway_queued(const char *mac,uint32_t batch,unsigned blocks,uint32_t wait)
{
    if(update_device_collection_batch_state(mac,"waiting",0,batch)!=0)
        fprintf(stderr,"[排队状态保存失败] MAC=%s | 批次=%u\n",mac,batch);
    pthread_mutex_lock(&g_ws_mutex);
    snprintf(g_ws_msg_buffer,sizeof(g_ws_msg_buffer),
             "{\"type\":\"vibration_upload_progress\",\"mac\":\"%s\",\"batch_id\":%u,"
             "\"collection_state\":\"waiting\",\"status\":\"queued\",\"received_packets\":0,"
             "\"total_packets\":%u,\"unit\":\"blocks\",\"wait_ms\":%u}",
             mac,batch,blocks,wait);
    GATEWAY_WS_EVENT_READY();
    pthread_mutex_unlock(&g_ws_mutex);
}
static void v2_gateway_remove(const char *mac)
{
    upload_remove(&v2_upload_admission,mac);
    report_forget(&v2_queue_reports,mac);
    for (unsigned i=0;i<V2_GATEWAY_SLOTS;++i)
        if (!strcmp(v2_gateway_slots[i].mac,mac))
        {
            v2_rx_reset(&v2_gateway_slots[i].rx);
            memset(&v2_gateway_slots[i],0,sizeof(v2_gateway_slots[i]));
        }
}
/* Host-side completion/repair replies use the same guarded UART path. */
static void v2_gateway_reply_for_query(V2GatewaySlot *slot,uint16_t short_addr,
                                       const uint8_t *reply,int query_seq)
{
    if(slot) {
        slot->last_reply_ms=v2_gateway_monotonic_ms();
        slot->fragment_pending=0;
        if(reply[8]==V2_MISSING)slot->first_block_had_missing=1;
    }
    if(reply[8]==V2_MISSING && reply[1]!=SCALAR_ACK_KIND)
        printf("🔁 [振动补传] 批次=%u | 第%u个窗口 | 缺片位图=0x%016llX，提交补传请求\n",
               v2_u32(reply+2),(unsigned)v2_u16(reply+6)+1U,
               (unsigned long long)v2_u64(reply+9));
#ifdef GATEWAY_UPLOAD_TRACE_SEND
    GATEWAY_UPLOAD_TRACE_SEND(short_addr,reply,query_seq);
#else
    uint8_t downlink[2+V2_ACK_LEN+1U];
    size_t length=2U+V2_ACK_LEN;
    v2_w16(downlink,short_addr);memcpy(downlink+2,reply,V2_ACK_LEN);
    if(query_seq>=0)downlink[length++]=(uint8_t)query_seq;
    pthread_mutex_lock(&g_uart_mutex);
    if(g_uart_fd>=0)send_hex_frame_to_uart(g_uart_fd,V2_ACK,length,downlink);
    pthread_mutex_unlock(&g_uart_mutex);
#endif
}

static void v2_gateway_reply(V2GatewaySlot *slot,uint16_t short_addr,const uint8_t *reply)
{
    v2_gateway_reply_for_query(slot,short_addr,reply,-1);
}

/* Completion is established from current durable raw/features/scalars, never
 * from MQTT status, a device shadow, or the now-released reservation. */
static int v2_gateway_complete_reply(const char *mac,uint16_t address,uint32_t batch,uint32_t nonce)
{
    UploadBatchReceipt_t receipt;
    if(get_completed_upload_batch(mac,batch,&receipt)!=1 ||
       !scalar_manifest_valid(receipt.manifest,SCALAR_MANIFEST_LEN) ||
       scalar_db_missing(mac,receipt.manifest))return 0;
    uint32_t proof=upload_completion_fingerprint(receipt.identity,receipt.manifest,receipt.raw_required);
    uint8_t reply[V2_ACK_LEN];
    v2_ack(reply,UPLOAD_COMPLETION_KIND,batch,0,V2_OK,(uint64_t)nonce|((uint64_t)proof<<32));
    v2_gateway_reply(NULL,address,reply);
    upload_release_reason(&v2_upload_admission,mac,batch,v2_gateway_monotonic_ms(),"数据库确认批次已完成，重放完成回复");
    printf("[批次完成重确认] MAC=%s | 批次=%u | %s | 数据库完整校验通过，无需重新排队或上传\n",
           mac,batch,nonce?"续期":"重新申请");
    return 1;
}

static void handle_scalar_manifest(const uint8_t *payload,size_t length)
{
    if(length!=2U+SCALAR_MANIFEST_LEN || !scalar_manifest_valid(payload+2,length-2))return;
    uint16_t address=v2_u16(payload);const uint8_t *manifest=payload+2;
    uint32_t batch=scalar_u32(manifest+1);char mac[24];
    pthread_mutex_lock(&g_vibration_mutex);
    if(!lookup_mac_by_short(address,mac) || !is_device_in_db(mac))goto done;
    uint8_t status=V2_NEED_BATCH,missing=0;
    V2GatewaySlot *slot=NULL;
    uint64_t now=v2_gateway_monotonic_ms();
    for(unsigned i=0;i<V2_GATEWAY_SLOTS;i++)
        if(v2_gateway_slots[i].batch_id==batch && !strcmp(v2_gateway_slots[i].mac,mac)) {
            slot=&v2_gateway_slots[i];break;
        }
    UploadAdmissionEntry *owner=slot?upload_owner_entry(&v2_upload_admission,mac,batch,now):NULL;
    if(owner) {
        if(slot->scalar_started && memcmp(slot->scalar_manifest,manifest,SCALAR_MANIFEST_LEN))
            status=V2_REJECT;
        else {
            memcpy(slot->scalar_manifest,manifest,SCALAR_MANIFEST_LEN);slot->scalar_started=1;
            uint8_t unavailable=manifest[6]&(uint8_t)~manifest[7];
            if(unavailable && !slot->scalar_unavailable_logged) {
                GATEWAY_SCALAR_UNAVAILABLE_EVENT(mac,address,batch,unavailable);
                slot->scalar_unavailable_logged=1;
            }
            {
                missing=scalar_db_missing(mac,manifest);
                status=missing?V2_MISSING:V2_OK;
                if(!missing && GATEWAY_SCALAR_POLICY_EVENT(mac,batch,manifest[6],unavailable)!=0)
                    status=V2_STORE_FAILED;
                if(status==V2_OK && remember_upload_manifest(mac,batch,manifest)!=0)
                    status=V2_STORE_FAILED;
                /* Keep the first complete verification within this upload permit. */
                if(status==V2_OK && (manifest[6]&manifest[7])==SCALAR_ALL && !owner->scalar_verified_ms)
                    owner->scalar_verified_ms=v2_gateway_monotonic_ms();
                printf("%s [批次数据校验] MAC=%s | 批次=%u | 本批应收=0x%02X | 缺失或校验不符=0x%02X | %s\n",
                       missing?"🔁":"✅",mac,batch,manifest[6],missing,
                       missing?"要求补发，振动暂不放行":status==V2_OK?
                       "已采到的数据已入库确认，允许振动上传":"批次策略保存失败，等待重试");
            }
            slot->scalar_ready=status==V2_OK;
            upload_progress(&v2_upload_admission,mac,batch,v2_rx_done_count(&slot->rx),now);
        }
        if(status==V2_REJECT)upload_revoke(&v2_upload_admission,mac,batch,now,"同批清单冲突，撤销许可并等待旧许可失效");
    }
    uint8_t reply[V2_ACK_LEN];v2_ack(reply,SCALAR_ACK_KIND,batch,0,status,missing);
    v2_gateway_reply(NULL,address,reply);
done:
    pthread_mutex_unlock(&g_vibration_mutex);
}
/* Wait for tail/reorder grace, or detect a lost final fragment by inactivity.
 * Never request repairs repeatedly while waiting for the node's first reply. */
static void v2_gateway_tick(void)
{
    uint64_t now=v2_gateway_monotonic_ms();
    pthread_mutex_lock(&g_vibration_mutex);
    if(!v2_gateway_boot_known){v2_gateway_boot_ms=now;v2_gateway_boot_known=1;}
    upload_expire(&v2_upload_admission,now);
    for(unsigned i=0;i<V2_GATEWAY_SLOTS;++i) {
        V2GatewaySlot *s=&v2_gateway_slots[i];
        if(!s->rx.have_block || !s->fragment_pending ||
           !upload_owner(&v2_upload_admission,s->mac,s->batch_id,now))continue;
        uint64_t grace=s->last_fragment_seen?200U:1500U;
        if(now-s->last_fragment_ms<grace || now-s->last_reply_ms<grace)continue;
        V2RxIO io={s->mac,v2_gateway_stored,v2_gateway_store,v2_gateway_progress};
        uint8_t reply[V2_ACK_LEN];
        if(v2_rx_window(&s->rx,&io,reply))v2_gateway_reply(s,s->short_addr,reply);
    }
    pthread_mutex_unlock(&g_vibration_mutex);
}

static void handle_vibration_v2(uint8_t command,const uint8_t *payload,size_t length)
{
    if (length<3) return;
    uint16_t short_addr=v2_u16(payload);
    const uint8_t *p=payload+2;size_t n=length-2;
    int query_seq=-1;
    if(v2_window_query_has_tag(command,p,n))query_seq=p[--n];
    /* Validate envelopes before deriving identities or allocating memory. */
    int completion_batch=command==V2_BATCH && n==UPLOAD_COMPLETION_BATCH_LEN &&
        p[V2_BATCH_LEN]==UPLOAD_COMPLETION_VERSION && p[V2_BATCH_LEN+1U]<=1U;
    int completion_lease=command==V2_QUERY && n==UPLOAD_COMPLETION_LEASE_LEN &&
        p[1]==UPLOAD_LEASE_KIND && p[10]==UPLOAD_COMPLETION_VERSION;
    if ((command==V2_BATCH && !v2_valid_batch(p,completion_batch?V2_BATCH_LEN:n)) ||
        (command==V2_BLOCK && (n!=V2_BLOCK_LEN || p[0]!=V2_VERSION)) ||
        (command==V2_DATA && (n<=7 || n>79)) ||
        (command==V2_QUERY && ((n!=8 && !(n==10 && p[1]==UPLOAD_LEASE_KIND) && !completion_lease) || p[0]!=V2_VERSION))) return;
    uint32_t batch=command==V2_DATA?v2_u32(p):
        command==V2_QUERY?v2_u32(p+2):v2_u32(p+1);
    if (!batch) return;
    pthread_mutex_lock(&g_vibration_mutex);
    char mac[24]={0};
    if (!lookup_mac_by_short(short_addr,mac) || !is_device_in_db(mac)) goto out;
    uint64_t request_now=v2_gateway_monotonic_ms();
    if(completion_batch) {
        if(remember_upload_batch(mac,p,p[V2_BATCH_LEN+1U])!=0) {
            uint8_t conflict[V2_ACK_LEN];v2_ack(conflict,V2_KIND_BATCH,batch,0,V2_REJECT,0);
            v2_gateway_reply(NULL,short_addr,conflict);
            goto out;
        }
        if(v2_gateway_complete_reply(mac,short_addr,batch,0))goto out;
        n=V2_BATCH_LEN;
    }
    if(completion_lease && v2_gateway_complete_reply(mac,short_addr,batch,v2_u32(p+6)))goto out;
    if(!v2_gateway_boot_known){v2_gateway_boot_ms=request_now;v2_gateway_boot_known=1;}
    if(command==V2_BATCH && request_now-v2_gateway_boot_ms<UPLOAD_RESTART_GUARD_MS) {
        uint8_t guard[V2_ACK_LEN];uint32_t wait=(uint32_t)(UPLOAD_RESTART_GUARD_MS-(request_now-v2_gateway_boot_ms));
        v2_ack(guard,V2_KIND_BATCH,batch,0,V2_WAIT,wait);
        v2_gateway_reply(NULL,short_addr,guard);
        v2_gateway_queued(mac,batch,v2_blocks(v2_u32(p+7)),wait);
        printf("[上传保护] 后端刚启动，等待旧许可失效 | MAC=%s | 批次=%u | 等待=%u ms\n",mac,batch,wait);
        goto out;
    }
    if(command==V2_QUERY && p[1]==UPLOAD_LEASE_KIND && (n==10 || completion_lease)) {
        uint8_t lease_reply[V2_ACK_LEN];
        int owner=upload_owner(&v2_upload_admission,mac,batch,request_now);
        if(owner)upload_progress(&v2_upload_admission,mac,batch,0,request_now);
        v2_ack(lease_reply,UPLOAD_LEASE_KIND,batch,0,owner?V2_OK:V2_NEED_BATCH,
               (uint64_t)v2_u32(p+6)|((uint64_t)UPLOAD_LEASE_MS<<32));
        v2_gateway_reply(NULL,short_addr,lease_reply);
        if(!owner)printf("[上传许可失效] MAC=%s | 批次=%u | 续期被拒，须重新申请 | nonce=%u\n",mac,batch,v2_u32(p+6));
        goto out;
    }
    if(command==V2_BATCH) {
        GATEWAY_MQTT_BATCH_EVENT(mac,batch,0);
        unsigned raw=completion_batch?p[V2_BATCH_LEN+1U]:1U;
        uint32_t wait=upload_request_kind(&v2_upload_admission,mac,batch,v2_blocks(v2_u32(p+7)),raw,request_now);
        if(wait) {
            uint8_t queued[V2_ACK_LEN];v2_ack(queued,V2_KIND_BATCH,batch,0,V2_WAIT,wait);
            v2_gateway_reply(NULL,short_addr,queued);
            /* Always reply, but coalesce retries within a 10-second burst.
             * A real later reapplication remains visible as a queue review. */
            if(!report_seen(&v2_queue_reports,mac,batch,V2_BATCH,NULL,0,request_now,10000U)) {
                int again=report_seen(&v2_queue_reports,mac,batch,V2_BATCH,NULL,0,request_now,0);
                printf("⏳ [%s] MAC=%s | 批次=%u | 预计等待=%u秒；唤醒后需重新申请许可\n",
                       again?"排队复查":"上传排队",mac,batch,(wait+999U)/1000U);
                v2_gateway_queued(mac,batch,v2_blocks(v2_u32(p+7)),wait);
                report_remember(&v2_queue_reports,mac,batch,V2_BATCH,NULL,0,request_now);
            }
            goto out;
        }
    } else if(!upload_owner(&v2_upload_admission,mac,batch,request_now)) {
        if(command!=V2_DATA) {
            uint8_t retry[V2_ACK_LEN];
            v2_ack(retry,command==V2_BLOCK?V2_KIND_BLOCK:p[1],batch,
                   command==V2_BLOCK?v2_u16(p+5):v2_u16(p+6),V2_NEED_BATCH,0);
            v2_gateway_reply_for_query(NULL,short_addr,retry,query_seq);
        }
        goto out;
    }
    V2GatewaySlot *slot=NULL,*spare=NULL;
    time_t now=time(NULL);
    for (unsigned i=0;i<V2_GATEWAY_SLOTS;++i)
    {
        V2GatewaySlot *s=&v2_gateway_slots[i];
        if (s->mac[0] && !strcmp(s->mac,mac) && s->batch_id==batch) {slot=s;break;}
        if (!s->mac[0] || s->rx.complete || (now>=s->touched && now-s->touched>300))
            if (!spare || !s->mac[0]) spare=s;
    }
    uint8_t reply[V2_ACK_LEN];int have_reply=0;
    if (!slot && command==V2_BATCH && spare)
    {
        v2_rx_reset(&spare->rx);memset(spare,0,sizeof(*spare));
        snprintf(spare->mac,sizeof(spare->mac),"%s",mac);spare->batch_id=batch;slot=spare;
    }
    if (slot)
    {
        if(command!=V2_BATCH && slot->scalar_started && !slot->scalar_ready) {
            if(command!=V2_DATA) {
                unsigned kind=command==V2_BLOCK?V2_KIND_BLOCK:p[1];
                v2_ack(reply,kind,batch,command==V2_BLOCK?v2_u16(p+5):v2_u16(p+6),V2_NEED_BATCH,0);
                v2_gateway_reply_for_query(NULL,short_addr,reply,query_seq);
            }
            goto out;
        }
        V2RxIO io={slot->mac,v2_gateway_stored,v2_gateway_store,v2_gateway_progress};
        slot->touched=now;slot->short_addr=short_addr;
        if(command==V2_BLOCK && slot->rx.block_index!=v2_u16(p+5)) {
            slot->fragment_pending=0;slot->last_fragment_seen=0;
        }
        /* Log valid received fragments BEFORE the decoder calls progress.
         * Timing is first received fragment to last, excluding CRC/SQL work;
         * reception order need not start with fragment zero. */
        if (command==V2_DATA && v2_u16(p+4)==0U &&
            slot->rx.have_block && slot->rx.block_index==0U &&
            !slot->rx.blocks_done[0] && p[6]<slot->rx.fragments &&
            n==7U+((slot->rx.encoded_len-(unsigned)p[6]*V2_FRAGMENT_BYTES)>V2_FRAGMENT_BYTES ?
                  V2_FRAGMENT_BYTES : slot->rx.encoded_len-(unsigned)p[6]*V2_FRAGMENT_BYTES))
        {
            if(!slot->first_block_timing_active && !slot->first_block_receive_logged) {
                GATEWAY_MQTT_BATCH_EVENT(mac,batch,1);
                char timestamp[32];
                slot->first_block_started_ms=v2_gateway_monotonic_ms();
                slot->first_block_timing_active=1;
                v2_gateway_wall_time(timestamp);
                printf("📦 [振动上传] MAC=%s | 批次=%u | 首个波形分片已到达，开始接收原始波形\n",mac,batch);
                printf("⏱️ [振动首窗口] MAC=%s | 批次=%u | 起点=%s | 压缩数据=%u字节 | 分片=%u包\n",
                       mac,batch,timestamp,(unsigned)slot->rx.encoded_len,(unsigned)slot->rx.fragments);
            }
            if(slot->first_block_timing_active && !slot->first_block_receive_logged &&
               (slot->rx.fragments_received|(UINT64_C(1)<<p[6]))==v2_mask(slot->rx.fragments)) {
                char timestamp[32];
                uint64_t elapsed=v2_gateway_monotonic_ms()-slot->first_block_started_ms;
                v2_gateway_wall_time(timestamp);
                printf("⏱️ [振动首窗口] MAC=%s | 批次=%u | %u包全部收到%s | 终点=%s | 耗时=%llu ms | 计时范围=首包到末包（校验前）\n",
                       mac,batch,(unsigned)slot->rx.fragments,
                       slot->first_block_had_missing ? "（含缺包补传）" : "（首轮无缺包）",
                       timestamp,(unsigned long long)elapsed);
                slot->first_block_timing_active=0;
                slot->first_block_receive_logged=1;
            }
        }
        have_reply=v2_rx_receive(&slot->rx,&io,command,p,
                                 command==V2_BLOCK?V2_BLOCK_LEN:n,reply);
        upload_progress(&v2_upload_admission,mac,batch,v2_rx_done_count(&slot->rx),request_now);
        if(command==V2_DATA && slot->rx.have_block &&
           slot->rx.block_index==v2_u16(p+4) && p[6]<slot->rx.fragments &&
           (slot->rx.fragments_received&(UINT64_C(1)<<p[6]))) {
            slot->last_fragment_ms=v2_gateway_monotonic_ms();
            slot->fragment_pending=1;
            if(p[6]+1U==slot->rx.fragments)slot->last_fragment_seen=1;
        }
        if (have_reply && command==V2_QUERY && p[1]==V2_KIND_WINDOW &&
            v2_u16(p+6)==0U && reply[8]==V2_MISSING)
            slot->first_block_had_missing=1;
    }
    else if (command!=V2_DATA)
    {
        unsigned kind=command==V2_BATCH?V2_KIND_BATCH:command==V2_BLOCK?V2_KIND_BLOCK:p[1];
        unsigned block=command==V2_BLOCK?v2_u16(p+5):command==V2_QUERY?v2_u16(p+6):0;
        v2_ack(reply,kind,batch,block,command==V2_BATCH?V2_WAIT:V2_NEED_BATCH,
               command==V2_BATCH?V2_QUEUE_RETRY_MS:0);
        have_reply=1;
    }
    if(have_reply) {
        v2_gateway_reply_for_query(slot,short_addr,reply,query_seq);
        /* Legacy batches with embedded features finish at FINAL. Deferred
         * features keep the reservation until their durable summary ACK. */
        if(reply[1]==V2_KIND_FINAL && reply[8]==V2_OK && slot) {
            int embedded=0;
            for(unsigned i=0;i<15;i++)if(v2_u32(slot->rx.batch+17+4*i)!=0x80000000U)embedded=1;
            if(embedded)upload_finish(&v2_upload_admission,mac,batch,request_now,"波形及嵌入特征完成，释放名额");
        }
        if(reply[8]==V2_REJECT)upload_revoke(&v2_upload_admission,mac,batch,request_now,"上传协议拒绝，撤销许可并等待旧许可失效");
    }
out:
    pthread_mutex_unlock(&g_vibration_mutex);
}
