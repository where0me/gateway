#ifndef GATEWAY_JOIN_H
#define GATEWAY_JOIN_H
/* A decoded node-originated status/query is sent after its startup completed.
 * Descriptor/model responses and announcements are not completion evidence. */
static void confirm_visible_node(const char *mac,uint16_t address,const char *reason)
{
    int result=confirm_device_join(mac,address);
    if(result>0) {
        printf("[节点通信确认] MAC=%s | 短地址=0x%04X | %s，等待用户授权\n",mac,(unsigned)address,reason);
        gateway_log_event("DEBUG","node","node_identified",mac,address,-1,
                          "节点通信已确认（%s），识别信息齐全后进入待授权列表",reason);
    } else if(result<0) {
        gateway_log_event("ERROR","backend","node_join_confirm_failed",mac,address,-1,
                          "节点入网确认保存失败，等待后续有效状态重试");
    }
}
/* Shared by HTTP and UART tasks; g_pending_mutex protects the observation. */
static uint64_t join_observed_at, join_expires_at, join_request_at;
static int join_requested=-1;
static const char *join_headers="Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n";
static int admission_is_open(void)
{
    uint64_t now=signal_now_ms();
    pthread_mutex_lock(&g_pending_mutex);
    int open=join_requested!=0 && join_observed_at && now-join_observed_at<8000 &&
        g_scan_state==1 && now<join_expires_at;
    pthread_mutex_unlock(&g_pending_mutex);return open;
}
static int admission_is_closed(void)
{
    uint64_t now=signal_now_ms();
    pthread_mutex_lock(&g_pending_mutex);
    int closed=join_requested==0 || (join_requested!=1 && join_observed_at &&
        now-join_observed_at<8000 && g_scan_state==0);
    pthread_mutex_unlock(&g_pending_mutex);return closed;
}
static void join_observe(const unsigned char *p,unsigned len)
{
    if(len!=2 || p[0]>1 || (p[0] && !p[1])) return;
    uint64_t now=signal_now_ms();
    pthread_mutex_lock(&g_pending_mutex);
    join_observed_at=now; join_expires_at=now+(uint64_t)p[1]*1000;
    g_scan_state=p[0];
    if(join_requested==g_scan_state) join_requested=-1;
    pthread_mutex_unlock(&g_pending_mutex);
}
static void join_tick(void)
{
    static uint64_t next_query;
    uint64_t now=signal_now_ms();
    if(now<next_query)return;
    next_query=now+3000;
    pthread_mutex_lock(&g_uart_mutex);
    if(g_uart_fd>=0)send_hex_frame_to_uart(g_uart_fd,0x13,0,NULL);
    pthread_mutex_unlock(&g_uart_mutex);
}
static void join_api(struct mg_connection *c,struct mg_http_message *hm,int request)
{
    if(mg_strcmp(hm->method,mg_str(request<0?"GET":"POST"))!=0) {
        mg_http_reply(c,405,join_headers,"{\"status\":\"error\"}"); return;
    }
    uint64_t now=signal_now_ms();
    if(request>=0) {
        pthread_mutex_lock(&g_pending_mutex);
        if(join_requested>=0 && now-join_request_at<6000) {
            pthread_mutex_unlock(&g_pending_mutex);
            mg_http_reply(c,409,join_headers,"{\"status\":\"error\",\"msg\":\"上一条入网请求尚未确认\"}"); return;
        }
        join_requested=request; join_request_at=now;
        pthread_mutex_unlock(&g_pending_mutex);
        /* Never nest the state lock inside the UART lock. */
        pthread_mutex_lock(&g_uart_mutex);
        int available=g_uart_fd>=0;
        if(available)send_hex_frame_to_uart(g_uart_fd,request?0x03:0x05,0,NULL);
        pthread_mutex_unlock(&g_uart_mutex);
        int cleanup=request?0:cancel_pending_admissions();
        if(cleanup<0) {
            mg_http_reply(c,500,join_headers,"{\"status\":\"error\",\"msg\":\"已请求关闭网络，但部分待授权节点清退保存失败，请重试禁止加入\"}");return;
        }
        if(!available) {
            pthread_mutex_lock(&g_pending_mutex);join_requested=-1;pthread_mutex_unlock(&g_pending_mutex);
            mg_http_reply(c,503,join_headers,"{\"status\":\"error\",\"msg\":\"协调器串口不可用\"}");return;
        }
        mg_http_reply(c,202,join_headers,"{\"status\":\"success\",\"pending\":true}"); return;
    }
    pthread_mutex_lock(&g_pending_mutex);
    int known=join_observed_at && now-join_observed_at<8000;
    int active=known && g_scan_state==1 && now<join_expires_at;
    unsigned remaining=active?(unsigned)((join_expires_at-now+999)/1000):0;
    int pending=join_requested>=0 && now-join_request_at<6000;
    int failed=join_requested>=0 && !pending;
    pthread_mutex_unlock(&g_pending_mutex);
    mg_http_reply(c,200,join_headers,
      "{\"known\":%s,\"active\":%s,\"remaining\":%u,\"pending\":%s,\"error\":%s}",
      known?"true":"false",active?"true":"false",remaining,pending?"true":"false",
      failed?"\"协调器未确认请求，请重试或检查固件版本\"":"null");
}
static void handle_interview(unsigned cmd,const unsigned char *p,unsigned len)
{
    if((cmd==0xA2 && len<12) || (cmd==0xA3 && len!=14))return;
    char mac[24],short_text[8];
    snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
      p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
    uint16_t address=(uint16_t)p[8]|((uint16_t)p[9]<<8), current;
    if(!get_device_short_addr(mac,&current) || current!=address)return;
    char *saved=load_device_interview(mac);
    cJSON *root=saved?cJSON_Parse(saved):NULL; free(saved);
    if(!root)root=cJSON_CreateObject();
    if(!root)return;
    if(cmd==0xA2) {
        static const char *states[]={"unknown","interviewing","standard","ready","failed"};
        if(p[10]>4) {cJSON_Delete(root);return;}
        cJSON_DeleteItemFromObject(root,"state"); cJSON_AddStringToObject(root,"state",states[p[10]]);
        if(len>12) {
            if(len<16 || len!=16U+2U*(p[14]+p[15])) {cJSON_Delete(root);return;}
            cJSON *eps=cJSON_GetObjectItem(root,"endpoints");
            if(!eps) {eps=cJSON_CreateObject();cJSON_AddItemToObject(root,"endpoints",eps);}
            char key[4];snprintf(key,sizeof(key),"%u",p[11]);
            cJSON *ep=cJSON_CreateObject(),*in=cJSON_CreateArray(),*out=cJSON_CreateArray();
            cJSON_AddNumberToObject(ep,"profile",(unsigned)p[12]|((unsigned)p[13]<<8));
            unsigned n=16;
            for(unsigned k=0;k<p[14];k++,n+=2)cJSON_AddItemToArray(in,cJSON_CreateNumber(p[n]|(p[n+1]<<8)));
            for(unsigned k=0;k<p[15];k++,n+=2)cJSON_AddItemToArray(out,cJSON_CreateNumber(p[n]|(p[n+1]<<8)));
            cJSON_AddItemToObject(ep,"input",in);cJSON_AddItemToObject(ep,"output",out);
            cJSON_DeleteItemFromObject(eps,key);cJSON_AddItemToObject(eps,key,ep);
        } else if(p[10]==1 && p[11]==0) {
            /* Fresh interview: no stale capabilities from previous firmware. */
            cJSON_DeleteItemFromObject(root,"endpoints");
            cJSON_DeleteItemFromObject(root,"protocol");cJSON_DeleteItemFromObject(root,"capabilities");
        }
    } else {
        cJSON_DeleteItemFromObject(root,"protocol");cJSON_DeleteItemFromObject(root,"capabilities");
        cJSON_AddNumberToObject(root,"protocol",p[10]|(p[11]<<8));
        cJSON_AddNumberToObject(root,"capabilities",p[12]|(p[13]<<8));
    }
    char *json=cJSON_PrintUnformatted(root);cJSON_Delete(root);
    if(json) { (void)save_device_interview(mac,json);free(json); }
    if(cmd==0xA2 && len==12U && p[10]==3U)
        confirm_visible_node(mac,address,"协调器已确认入网后的应用上行");
    snprintf(short_text,sizeof(short_text),"%04X",address);
    update_device_online(mac,short_text);
}
static void interview_retry_api(struct mg_connection *c,struct mg_http_message *hm)
{
    if(mg_strcmp(hm->method,mg_str("POST"))!=0) {mg_http_reply(c,405,join_headers,"{}");return;}
    cJSON *root=cJSON_ParseWithLength(hm->body.buf,hm->body.len);
    cJSON *mac=cJSON_GetObjectItem(root,"mac"); uint16_t address;
    if(!cJSON_IsString(mac) || !get_device_short_addr(mac->valuestring,&address)) {
        cJSON_Delete(root);mg_http_reply(c,400,join_headers,"{\"msg\":\"节点尚无有效网络地址\"}");return;
    }
    cJSON_Delete(root);
    unsigned char p[2]={(unsigned char)address,(unsigned char)(address>>8)};
    pthread_mutex_lock(&g_uart_mutex);
    int available=g_uart_fd>=0;
    if(available)send_hex_frame_to_uart(g_uart_fd,0x14,2,p);
    pthread_mutex_unlock(&g_uart_mutex);
    mg_http_reply(c,available?202:503,join_headers,"{\"status\":\"%s\"}",available?"success":"error");
}
#endif
