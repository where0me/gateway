#ifndef ADMISSION_PROBE_H
#define ADMISSION_PROBE_H
/* HTTP-loop scheduling, UART-thread replies. Never hold a DB lock when entering
 * this mutex. A status heartbeat is not a reply to an outstanding probe. */
typedef struct AdmissionProbe {
    struct AdmissionProbe *next;
    char mac[32];
    uint16_t address, token;
    unsigned misses, visible, listed, seen;
    uint64_t due, deadline;
} AdmissionProbe;
static AdmissionProbe *admission_probes;
static pthread_mutex_t admission_probe_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint16_t admission_probe_sequence;
static uint32_t admission_probe_random;
static uint64_t admission_probe_next_send;

static uint32_t admission_probe_delay(uint64_t now)
{
    if(!admission_probe_random)admission_probe_random=(uint32_t)now ^ 0x9e3779b9U;
    admission_probe_random^=admission_probe_random<<13;
    admission_probe_random^=admission_probe_random>>17;
    admission_probe_random^=admission_probe_random<<5;
    return 3000U+admission_probe_random%7001U;
}
static AdmissionProbe *admission_probe_find(const char *mac)
{
    for(AdmissionProbe *p=admission_probes;p;p=p->next)if(!strcmp(p->mac,mac))return p;
    return NULL;
}
static void admission_probe_clear(void)
{
    pthread_mutex_lock(&admission_probe_mutex);
    while(admission_probes){AdmissionProbe *p=admission_probes;admission_probes=p->next;free(p);}
    admission_probe_next_send=0;
    pthread_mutex_unlock(&admission_probe_mutex);
}
static int admission_probe_visible(const char *mac)
{
    pthread_mutex_lock(&admission_probe_mutex);
    AdmissionProbe *p=admission_probe_find(mac);uint16_t address;
    int visible=p && p->visible && get_device_short_addr(mac,&address) && p->address==address;
    pthread_mutex_unlock(&admission_probe_mutex);return visible;
}
/* Call only after the API's join/metadata/signal readiness checks. Initial
 * listing uses that existing proof; probing starts after this first listing.
 * A later GET must never undo the three-timeout hidden state. */
static int admission_probe_list(const char *mac)
{
    uint64_t now=signal_now_ms();
    pthread_mutex_lock(&admission_probe_mutex);
    AdmissionProbe *p=admission_probe_find(mac);uint16_t address;
    int visible=0;
    if(p && get_device_short_addr(mac,&address) && p->address==address) {
        if(!p->listed) {
            p->listed=1U;p->visible=1U;p->misses=0U;p->deadline=0;
            p->due=now+admission_probe_delay(now);
            printf("[待授权列表] MAC=%s | 入网通信及识别信息齐全，首次展示；后续开始主动探测\n",mac);
        }
        visible=p->visible;
    }
    pthread_mutex_unlock(&admission_probe_mutex);return visible;
}
/* Also used before a manual refresh, so newly discovered/hidden candidates are
 * included. Volatile entries restart with ordinary first-listing readiness. */
static void admission_probe_sync(void)
{
    DeviceRecord_t *records=NULL;int count=get_all_devices(&records);
    if(count<0)return;
    pthread_mutex_lock(&admission_probe_mutex);
    for(AdmissionProbe *p=admission_probes;p;p=p->next)p->seen=0;
    for(int i=0;i<count;i++) {
        DeviceRecord_t *r=records+i;uint16_t address;
        if(r->user_authorized || !get_device_short_addr(r->mac,&address))continue;
        AdmissionProbe *p=admission_probe_find(r->mac);
        if(!p){p=calloc(1,sizeof(*p));if(!p)continue;
            snprintf(p->mac,sizeof(p->mac),"%s",r->mac);p->address=address;
            p->next=admission_probes;admission_probes=p;}
        if(p->address!=address){p->address=address;p->visible=0;p->listed=0;p->misses=0;p->deadline=0;p->due=0;}
        p->seen=1;
    }
    AdmissionProbe **link=&admission_probes;
    while(*link){AdmissionProbe *p=*link;if(!p->seen){*link=p->next;free(p);}else link=&p->next;}
    pthread_mutex_unlock(&admission_probe_mutex);free(records);
}
static void admission_probe_refresh(void)
{
    admission_probe_sync();uint64_t now=signal_now_ms();
    pthread_mutex_lock(&admission_probe_mutex);
    for(AdmissionProbe *p=admission_probes;p;p=p->next)if(p->listed && !p->deadline)p->due=now;
    pthread_mutex_unlock(&admission_probe_mutex);
}
static void admission_probe_tick(void)
{
    static uint64_t next_sync;
    if(!admission_is_open()){if(admission_is_closed())admission_probe_clear();return;}
    uint64_t now=signal_now_ms();
    if(now>=next_sync){admission_probe_sync();next_sync=now+500U;}
    pthread_mutex_lock(&admission_probe_mutex);
    AdmissionProbe *selected=NULL;
    for(AdmissionProbe *p=admission_probes;p;p=p->next) {
        if(!p->listed)continue;
        if(p->deadline && now>=p->deadline) {
            p->deadline=0;if(p->misses<3U)p->misses++;
            if(p->misses==3U && p->visible) {
                p->visible=0;
                printf("[待授权探测] MAC=%s | 连续3次无有效回复，暂时隐藏，保留后台记录\n",p->mac);
            }
            p->due=now+admission_probe_delay(now);
        }
        if(!p->deadline && now>=p->due && (!selected || p->due<selected->due))selected=p;
    }
    if(selected && now>=admission_probe_next_send) {
        pthread_mutex_lock(&g_uart_mutex);
        if(g_uart_fd>=0) {
            if(++admission_probe_sequence==0)++admission_probe_sequence;
            selected->token=admission_probe_sequence;
            unsigned char request[4]={(uint8_t)selected->address,(uint8_t)(selected->address>>8),
                (uint8_t)selected->token,(uint8_t)(selected->token>>8)};
            if(!send_hex_frame_to_uart_checked(g_uart_fd,0x10,sizeof(request),request))
                selected->deadline=now+3000U;
            else selected->due=now+admission_probe_delay(now); /* Local failure is not a missing peer reply. */
            admission_probe_next_send=now+150U;
        }
        pthread_mutex_unlock(&g_uart_mutex);
    }
    pthread_mutex_unlock(&admission_probe_mutex);
}
static void admission_probe_reply(const uint8_t *data,unsigned length)
{
    /* Extended coordinator response: address, token, RSSI, LQI. Legacy five-byte
     * replies cannot prove fresh LQI and must not make candidates visible. */
    if(length!=6U || (int8_t)data[4]==-128)return;
    uint16_t address=data[0]|((uint16_t)data[1]<<8),token=data[2]|((uint16_t)data[3]<<8);
    uint64_t now=signal_now_ms();
    pthread_mutex_lock(&admission_probe_mutex);
    for(AdmissionProbe *p=admission_probes;p;p=p->next)if(p->address==address && p->token==token &&
        p->deadline && now<p->deadline) {
        uint16_t current;
        if(!device_is_authorized(p->mac) && get_device_short_addr(p->mac,&current) && current==address &&
            !update_pending_probe_telemetry(p->mac,data[5],(int8_t)data[4])) {
            confirm_visible_node(p->mac,address,"主动探测回复");
            p->visible=1;p->misses=0;p->deadline=0;p->due=now+admission_probe_delay(now);
        }
        break;
    }
    pthread_mutex_unlock(&admission_probe_mutex);
}
#endif
