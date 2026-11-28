#ifndef MANUAL_ADMISSION_API_H
#define MANUAL_ADMISSION_API_H
static int cancel_pending_admissions(void)
{
    admission_probe_clear();
    DeviceRecord_t *records=NULL;int count=get_all_devices(&records),errors=0;
    if(count<0)return -1;
    for(int i=0;i<count;i++) {
        DeviceRecord_t *r=records+i;if(r->user_authorized || r->sampling_admission==1)continue;
        uint16_t address=0;int have_address=get_device_short_addr(r->mac,&address);
        int result=cancel_unapproved_device(r->mac,have_address?(int)address:-1);
        if(result<0){errors++;continue;}
        if(result!=0)continue; /* Concurrent grant wins; never evict approved nodes. */
        unsigned char bytes[8];int sent=0;
        if(!mac_string_to_bytes(r->mac,bytes))
            sent=submit_removed_device_leave(bytes);
        gateway_log_event("INFO","frontend","admission_cancelled",r->mac,have_address?address:-1,-1,
            "停止允许加入，撤销未授权候选；离网请求%s，最终结果以协调器确认为准",sent?"已提交":"等待节点上行重试");
    }
    free(records);return errors?-1:0;
}
/* A click records durable intent; the node's applied status completes it. */
static void manual_admission_api(struct mg_connection *c,struct mg_http_message *hm,int grant)
{
    if(mg_strcmp(hm->method,mg_str(grant?"POST":"GET"))) {
        mg_http_reply(c,405,join_headers,"{}");return;
    }
    if(grant) {
        if(!admission_is_open()) {
            mg_http_reply(c,409,join_headers,"{\"status\":\"error\",\"msg\":\"当前未允许节点加入，无法确认入网\"}");return;
        }
        cJSON *body=cJSON_ParseWithLength(hm->body.buf,hm->body.len);
        cJSON *item=cJSON_GetObjectItem(body,"mac");
        unsigned char bytes[8];char mac[24];
        if(!cJSON_IsString(item) || mac_string_to_bytes(item->valuestring,bytes)) {
            cJSON_Delete(body);mg_http_reply(c,400,join_headers,"{\"msg\":\"MAC地址无效\"}");return;
        }
        snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
            bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7]);
        cJSON_Delete(body);
        int result=(!device_is_authorized(mac) && !device_admission_requested(mac) &&
            !admission_probe_visible(mac))?-2:request_device_admission(mac);
        if(result<0) {
            mg_http_reply(c,result==-2?409:500,join_headers,
                "{\"status\":\"error\",\"msg\":\"节点识别未完成或授权保存失败，请稍后重试\"}");return;
        }
        if(result>0)send_admission_grant(mac,1);
        int authorized=device_is_authorized(mac);
        mg_http_reply(c,authorized?200:202,join_headers,
            "{\"status\":\"success\",\"authorized\":%s}",authorized?"true":"false");
        return;
    }
    admission_probe_sync();
    DeviceRecord_t *records=NULL;int count=get_all_devices(&records);
    cJSON *list=cJSON_CreateArray();
    for(int i=0;i<count;i++) {
        DeviceRecord_t *r=records+i;
        if(!admission_is_open() || r->user_authorized || !device_admission_ready(r->mac) ||
            !admission_probe_list(r->mac))continue;
        cJSON *item=cJSON_CreateObject();
        cJSON_AddStringToObject(item,"name",r->name);
        cJSON_AddStringToObject(item,"model_name",r->model_name);
        cJSON_AddStringToObject(item,"mac",r->mac);
        cJSON_AddNumberToObject(item,"lqi",r->lqi);
        cJSON_AddNumberToObject(item,"rssi",r->rssi);
        if(r->sampling_admission==1)cJSON_AddBoolToObject(item,"authorizing",1);
        cJSON_AddItemToArray(list,item);
    }
    free(records);char *json=cJSON_PrintUnformatted(list);cJSON_Delete(list);
    mg_http_reply(c,json?200:500,join_headers,"%s",json?json:"[]");free(json);
}

static void manual_admission_refresh_api(struct mg_connection *c,struct mg_http_message *hm)
{
    if(mg_strcmp(hm->method,mg_str("POST"))){mg_http_reply(c,405,join_headers,"{}");return;}
    if(!admission_is_open()){mg_http_reply(c,409,join_headers,"{\"msg\":\"当前未允许节点加入\"}");return;}
    if(g_uart_fd<0){mg_http_reply(c,503,join_headers,"{\"msg\":\"协调器串口不可用\"}");return;}
    admission_probe_refresh();admission_probe_tick();
    mg_http_reply(c,202,join_headers,"{\"status\":\"success\"}");
}

static void manual_admission_tick(void)
{
    admission_probe_tick();
    static uint64_t next;static unsigned cursor;
    uint64_t now=signal_now_ms();if(now<next)return;next=now+5000;
    int closed=admission_is_closed();
    if(closed)(void)cancel_pending_admissions();
    DeviceRecord_t *records=NULL;int count=get_all_devices(&records);
    for(int i=0;i<count;i++)
        if(records[i].sampling_admission==1)send_admission_grant(records[i].mac,0);
    if(closed){free(records);return;}
    for(int k=0;k<count;k++) {
        DeviceRecord_t *r=records+((cursor+(unsigned)k)%(unsigned)count);
        if(r->user_authorized || (r->model_name[0] && strcmp(r->model_name,"Unknown") &&
            strcmp(r->model_name,"Unknown Device")))continue;
        uint16_t address;if(!get_device_short_addr(r->mac,&address))continue;
        unsigned char p[2]={(uint8_t)address,(uint8_t)(address>>8)};
        pthread_mutex_lock(&g_uart_mutex);
        if(g_uart_fd>=0)send_hex_frame_to_uart(g_uart_fd,0x04,2,p);
        pthread_mutex_unlock(&g_uart_mutex);
        cursor=(cursor+(unsigned)k+1U)%(unsigned)count;break;
    }
    free(records);
}
#endif
