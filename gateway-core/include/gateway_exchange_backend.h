#ifndef GATEWAY_EXCHANGE_BACKEND_H
#define GATEWAY_EXCHANGE_BACKEND_H

#define GX_RETRY_MS 2000U
#define GX_RETRY_LIFETIME_MS 60000U
#define GX_COMMAND_RECORD_SIZE (GX_COMMAND_FRAME_SIZE+1U)

/* Owned by the UART reader. Persist the immutable request before sending so
 * a restarted gateway can still match a cached final response from the node. */
typedef struct {
    char mac[32];
    uint64_t seen_at,retry_at,retry_until;
    uint8_t payload[GX_CONTEXT_SIZE+GX_SCALAR_SIZE];
    uint8_t command[GX_COMMAND_RECORD_SIZE];
} GatewayExchangeContext;

static GatewayExchangeContext gx_contexts[128];

static int gx_send(uint16_t address,const uint8_t *frame,unsigned size)
{
    uint8_t packet[2U+GX_COMMAND_FRAME_SIZE];
    gx_put16(packet,address);
    memcpy(packet+2,frame,size);
    pthread_mutex_lock(&g_uart_mutex);
    int sent=g_uart_fd>=0 && send_hex_frame_to_uart_checked(g_uart_fd,GX_UART_DOWN,
                                                           (unsigned char)(size+2U),packet)==0;
    pthread_mutex_unlock(&g_uart_mutex);
    return sent;
}

static int gx_decide(const char *mac,uint16_t address,const uint8_t *key,uint8_t result,uint16_t mask)
{
    uint8_t frame[GX_HEADER+GX_DECISION_SIZE];
    uint16_t seq;
    if (!next_gateway_exchange_sequence(mac,gx_get32(key),&seq)) return 0;
    gx_header(frame,GX_RESPONSE,GX_READY,seq,GX_DECISION_SIZE);
    memcpy(frame+GX_HEADER,key,GX_KEY_SIZE);
    frame[GX_HEADER+12]=result;
    gx_put16(frame+GX_HEADER+13,mask);
    return gx_send(address,frame,sizeof(frame));
}

static void gx_note_applied(const char *mac,const MonitorConfig_t *config)
{
    (void)update_device_expected_interval(mac,config->auto_interval_sec);
    if (g_mqtt_store_ready)
        (void)mqtt_store_policy(g_db,mac,config->sensor_enable_mask);
}

static void gx_schedule_retry(GatewayExchangeContext *ctx,uint64_t now,int immediate)
{
    ctx->retry_at=immediate?now:now+GX_RETRY_MS;
    ctx->retry_until=now+GX_RETRY_LIFETIME_MS;
}

static void gx_retry_tick(void)
{
    static uint64_t next_scan;
    uint64_t now=mg_millis();
    if (now<next_scan) return;
    next_scan=now+250U;
    for (unsigned i=0;i<sizeof(gx_contexts)/sizeof(gx_contexts[0]);i++) {
        GatewayExchangeContext *ctx=&gx_contexts[i];
        if (!ctx->retry_at || now<ctx->retry_at) continue;
        uint16_t address;
        if (now>=ctx->retry_until || !device_is_authorized(ctx->mac) ||
            !get_device_short_addr(ctx->mac,&address)) {
            ctx->retry_at=0U;
            continue;
        }
        (void)gx_send(address,ctx->command,GX_COMMAND_FRAME_SIZE);
        ctx->retry_at=now+GX_RETRY_MS;
    }
}

static int gx_issue_command(GatewayExchangeContext *ctx,const uint8_t *key,
                            const uint8_t *policy,uint64_t now,int immediate)
{
    uint8_t command[GX_COMMAND_RECORD_SIZE]={0};
    uint32_t token=gx_get32(key);
    uint16_t seq;
    if (!next_gateway_exchange_sequence(ctx->mac,token,&seq)) return 0;
    gx_header(command,GX_COMMAND,GX_SAMPLING,seq,GX_SAMPLING_SIZE);
    memcpy(command+GX_HEADER,key,GX_KEY_SIZE);
    memcpy(command+GX_HEADER+GX_KEY_SIZE,policy,CONTROL_CONFIG_SIZE);
    if (gateway_exchange_command_record(ctx->mac,token,command,sizeof(command),1)!=1) return 0;
    memcpy(ctx->command,command,sizeof(command));
    gx_schedule_retry(ctx,now,immediate);
    return 1;
}

static void gx_handle_final(GatewayExchangeContext *ctx,uint16_t address,const uint8_t *frame,uint64_t now)
{
    const uint8_t *body=frame+GX_HEADER;
    uint8_t result=body[8];
    uint32_t token=gx_get32(body),revision=gx_get32(body+9);
    uint32_t current=gx_get32(ctx->payload);
    if (current && current!=token) return;
    uint8_t attempt[GX_COMMAND_RECORD_SIZE];
    if (result>GX_SAVE_FAILED ||
        gateway_exchange_command_record(ctx->mac,token,attempt,sizeof(attempt),0)!=1 ||
        !gx_frame(attempt,GX_COMMAND_FRAME_SIZE,GX_COMMAND,GX_SAMPLING,GX_SAMPLING_SIZE) ||
        memcmp(attempt+GX_HEADER,body,8)) return;
    if (gx_get16(attempt+4)!=gx_get16(frame+4)) {
        /* A cached result for the previous command may arrive after restart.
         * Re-arm the newer, already-persisted request instead of replacing it. */
        if (!attempt[GX_COMMAND_FRAME_SIZE] && gx_newer(gx_get16(attempt+4),gx_get16(frame+4)) &&
            gx_get32(attempt+GX_HEADER+8)==revision &&
            (!ctx->retry_at || memcmp(ctx->command,attempt,sizeof(attempt)))) {
            memcpy(ctx->command,attempt,sizeof(attempt));
            gx_schedule_retry(ctx,now,0);
        }
        return;
    }
    uint32_t requested=gx_get32(attempt+GX_HEADER+8);
    if (revision!=(result?requested:requested+1U)) return;
    ctx->retry_at=0U;
    ctx->seen_at=now;
    if (!current) gx_put32(ctx->payload,token);
    uint8_t key[GX_KEY_SIZE];
    memcpy(key,body,8);
    gx_put32(key+8,revision);
    if (attempt[GX_COMMAND_FRAME_SIZE]) {
        if (!result) (void)gx_decide(ctx->mac,address,key,0U,GX_SLEEP_ALLOWED);
        return;
    }

    MonitorConfig_t config=load_monitor_config_for_mac(ctx->mac);
    uint8_t desired[31];char resolved[32];
    if (!build_monitor_config_payload(address,&config,desired,resolved) || strcmp(ctx->mac,resolved)) return;
    int changed=memcmp(desired+2,attempt+GX_HEADER+GX_KEY_SIZE,CONTROL_CONFIG_SIZE)!=0;
    if (changed || result==GX_SAVE_FAILED || result==GX_BUSY || result==GX_NOT_READY) {
        (void)gx_issue_command(ctx,key,desired+2,now,!result);
        return;
    }
    if (result) return;
    if (desired[28]&0x01U) return; /* Await channel migration while the command window stays open. */

    /* Persist the decision to finish before sending. A replay after this point
     * can only replay permission, even if a user saves a new policy meanwhile. */
    attempt[GX_COMMAND_FRAME_SIZE]=1U;
    if (gateway_exchange_command_record(ctx->mac,token,attempt,sizeof(attempt),1)==1) {
        gx_note_applied(ctx->mac,&config);
        (void)gx_decide(ctx->mac,address,key,0U,GX_SLEEP_ALLOWED);
    }
}

static void handle_gateway_exchange(const uint8_t *packet,unsigned length)
{
    if (!packet || length<2U+GX_HEADER) return;
    uint16_t address=gx_get16(packet);
    const uint8_t *frame=packet+2,*body=frame+GX_HEADER;
    unsigned n=length-2U;
    int scalar=gx_frame(frame,n,GX_EVENT,GX_CONTEXT,GX_CONTEXT_SIZE+GX_SCALAR_SIZE);
    int context=scalar || gx_frame(frame,n,GX_EVENT,GX_CONTEXT,GX_CONTEXT_SIZE);
    int final=gx_frame(frame,n,GX_RESPONSE,GX_SAMPLING,GX_SAMPLING_RESULT_SIZE);
    if (!context && !final && !gx_frame(frame,n,GX_EVENT,GX_READY,GX_READY_SIZE)) return;
    uint32_t token=gx_get32(body),batch=gx_get32(body+4);
    char mac[32];
    if (!token || !lookup_mac_by_short(address,mac) || !device_is_authorized(mac)) return;

    unsigned slot=0;
    for (unsigned i=0;i<sizeof(gx_contexts)/sizeof(gx_contexts[0]);i++) {
        if (!strcmp(gx_contexts[i].mac,mac)) { slot=i;break; }
        if (gx_contexts[i].seen_at<gx_contexts[slot].seen_at) slot=i;
    }
    GatewayExchangeContext *ctx=&gx_contexts[slot];
    if (strcmp(ctx->mac,mac)) {
        memset(ctx,0,sizeof(*ctx));
        snprintf(ctx->mac,sizeof(ctx->mac),"%s",mac);
    }
    uint64_t now=mg_millis();
    uint8_t *saved=ctx->payload;
    if (final) {
        gx_handle_final(ctx,address,frame,now);
        return;
    }
    if (context) {
        if ((body[16]&~(GX_CONTEXT_SCALAR|GX_CONTEXT_COMMANDS|GX_CONTEXT_FINAL)) ||
            !!(body[16]&GX_CONTEXT_SCALAR)!=scalar ||
            (scalar && (!scalar_manifest_valid(body+GX_CONTEXT_SIZE,GX_SCALAR_SIZE) ||
             gx_get32(body+GX_CONTEXT_SIZE+1)!=batch))) return;
        if (gx_get32(saved)==token && gx_get32(saved+8)>gx_get32(body+8)) return;
        if (ctx->retry_at && gx_get32(ctx->command+GX_HEADER)!=token) ctx->retry_at=0U;
        ctx->seen_at=now;
        memcpy(saved,body,n-GX_HEADER);
        return;
    }
    if (body[12]>GX_SAVE_FAILED || now-ctx->seen_at>10000U || memcmp(body,saved,GX_KEY_SIZE)) return;

    refresh_device_activity_by_short(address);
    channel_migration_note_node_awake(address);
    uint8_t result=0;
    if (batch && (saved[16]&GX_CONTEXT_SCALAR)) {
        if (scalar_db_missing(mac,saved+GX_CONTEXT_SIZE)) result=GX_NOT_READY;
    } else if (batch) {
        UploadBatchReceipt_t receipt;
        int rc=get_completed_upload_batch(mac,batch,&receipt);
        if (rc<0) return;
        if (rc!=1 || !scalar_manifest_valid(receipt.manifest,SCALAR_MANIFEST_LEN) ||
            scalar_db_missing(mac,receipt.manifest)) result=GX_NOT_READY;
    }
    if (result) {
        ctx->retry_at=0U;
        (void)gx_decide(mac,address,body,result,0U);
        return;
    }

    MonitorConfig_t config=load_monitor_config_for_mac(mac);
    uint8_t desired[31];char resolved[32];
    if (!build_monitor_config_payload(address,&config,desired,resolved) || strcmp(mac,resolved)) return;
    uint32_t hash=control_fingerprint(desired+2,CONTROL_CONFIG_SIZE);
    int change=hash!=gx_get32(saved+12) || body[12]!=0U;
    int migrating=(desired[28]&0x01U)!=0U;
    if (saved[16]&GX_CONTEXT_FINAL) {
        uint8_t attempt[GX_COMMAND_RECORD_SIZE];
        int rc=gateway_exchange_command_record(mac,token,attempt,sizeof(attempt),0);
        if (rc<0) return;
        if (rc==1) {
            if (!attempt[GX_COMMAND_FRAME_SIZE] &&
                gx_frame(attempt,GX_COMMAND_FRAME_SIZE,GX_COMMAND,GX_SAMPLING,GX_SAMPLING_SIZE) &&
                !memcmp(attempt+GX_HEADER,body,GX_KEY_SIZE) &&
                (!ctx->retry_at || memcmp(ctx->command,attempt,sizeof(attempt)))) {
                memcpy(ctx->command,attempt,sizeof(attempt));
                gx_schedule_retry(ctx,now,1);
            }
            return;
        }
        if (change && (saved[16]&GX_CONTEXT_COMMANDS)) {
            (void)gx_issue_command(ctx,body,desired+2,now,1);
            return;
        }
    } else if (change && (saved[16]&GX_CONTEXT_COMMANDS)) {
        /* Previous firmware reports a new READY after applying the policy. */
        uint8_t command[GX_COMMAND_FRAME_SIZE];
        gx_header(command,GX_COMMAND,GX_SAMPLING,1U,GX_SAMPLING_SIZE);
        memcpy(command+GX_HEADER,body,GX_KEY_SIZE);
        memcpy(command+GX_HEADER+GX_KEY_SIZE,desired+2,CONTROL_CONFIG_SIZE);
        (void)gx_send(address,command,sizeof(command));
        return;
    }
    uint16_t mask=change?GX_COMMANDS_PENDING:migrating?0U:GX_SLEEP_ALLOWED;
    if (gx_decide(mac,address,body,0U,mask) && mask==GX_SLEEP_ALLOWED)
        gx_note_applied(mac,&config);
}

#endif
