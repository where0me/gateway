#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "radio_diag.h"
static int g_radio_diag_enabled,g_uart_fd=3,g_uart_mutex,writes,stored;
static unsigned char switch_value;
static long long saved_batch;
static int saved_address;
static char saved_event[64],saved_message[1200],saved_mac[24],saved_level[16];
static void pthread_mutex_lock(int *m){assert(!*m);*m=1;}
static void pthread_mutex_unlock(int *m){assert(*m);*m=0;}
static void send_hex_frame_to_uart(int fd,unsigned char cmd,unsigned char len,const unsigned char *p)
{assert(fd==3 && g_uart_mutex && cmd==RADIO_DIAG_CMD && len==1);switch_value=*p;writes++;}
static int lookup_mac_by_short(uint16_t address,char mac[24])
{if(address!=0xB905)return 0;strcpy(mac,"9D:2C:EF:00:25:E1:80:00");return 1;}
static void gateway_log_event(const char *level,const char *source,const char *event,const char *mac,
                             int address,long long batch,const char *format,...)
{
    assert(!strcmp(source,"coordinator"));stored++;saved_address=address;saved_batch=batch;
    snprintf(saved_mac,sizeof(saved_mac),"%s",mac?mac:"");snprintf(saved_event,sizeof(saved_event),"%s",event);
    snprintf(saved_level,sizeof(saved_level),"%s",level);
    va_list args;va_start(args,format);vsnprintf(saved_message,sizeof(saved_message),format,args);va_end(args);
}
static void upload_trace_dump(uint16_t addr,uint32_t batch,unsigned reason,int ask)
{assert(addr==0xB905 && batch && reason==1 && !ask);}
static void request_radio_diagnostics(void);
static void handle_radio_diagnostic(const unsigned char *,unsigned char);
int main(void)
{
    request_radio_diagnostics();assert(writes==1 && !switch_value);
    g_radio_diag_enabled=1;request_radio_diagnostics();assert(writes==2 && switch_value==1);
    g_uart_fd=-1;request_radio_diagnostics();assert(writes==2);
    uint8_t p[RADIO_DIAG_LEN];radio_diag_base(p,RADIO_DIAG_COVERAGE,1000,0xFFFF);p[19]=2;
    g_radio_diag_enabled=0;handle_radio_diagnostic(p,40);assert(!stored);
    g_radio_diag_enabled=1;handle_radio_diagnostic(p,40);
    assert(stored==1 && saved_address==-1 && saved_batch==-1 && !strcmp(saved_level,"INFO"));
    assert(strstr(saved_message,"覆盖不完整") && !strcmp(saved_event,"radio_diag_coverage"));
    radio_w32(p+2,2000);handle_radio_diagnostic(p,40);assert(stored==1);
    radio_w32(p+2,5);handle_radio_diagnostic(p,40);assert(stored==2); // coordinator reboot visible
    radio_diag_base(p,RADIO_DIAG_AF_RESULT,3000,0xB905);p[19]=RADIO_DIAG_AF_ID_VALID;p[8]=90;p[9]=77;
    p[10]=3;p[11]=1;radio_w16(p+16,25);radio_w32(p+12,2850209704U);
    handle_radio_diagnostic(p,40);assert(stored==2); // high-frequency success stdout only
    p[18]=0xF0;handle_radio_diagnostic(p,40);
    assert(stored==3 && saved_batch==2850209704LL && saved_address==0xB905);
    assert(!strcmp(saved_mac,"9D:2C:EF:00:25:E1:80:00") && !strcmp(saved_level,"WARN"));
    assert(strstr(saved_message,"窗口=26") && strstr(saved_message,"AF事务=90") && strstr(saved_message,"不能据此判定"));
    assert(!strcmp(saved_event,"upload_ack_result"));
    p[1]=RADIO_DIAG_POLL;p[18]=0;handle_radio_diagnostic(p,40);assert(stored==3);
    radio_diag_base(p,RADIO_DIAG_MAC_RESULT,3010,0xB905);p[18]=0xE9;p[19]=RADIO_DIAG_HAS_REQ;
    handle_radio_diagnostic(p,40);assert(stored==4 && saved_batch==-1 && !strcmp(saved_event,"radio_mac_result"));
    assert(!strstr(saved_message,"APS未收到确认"));
    radio_diag_base(p,RADIO_DIAG_DROPPED,4000,0xFFFF);radio_w32(p+24,37);
    handle_radio_diagnostic(p,40);assert(stored==5 && strstr(saved_message,"37"));
    handle_radio_diagnostic(p,39);assert(stored==5);
    p[1]=99;handle_radio_diagnostic(p,40);assert(stored==5);
    g_radio_diag_enabled=0;
    radio_diag_base(p,RADIO_DIAG_AF_RESULT,5000,0xB905);
    p[10]=3;p[11]=1;radio_w16(p+16,11);radio_w32(p+12,3332029996U);
    handle_radio_diagnostic(p,40);assert(stored==5); // quiet success
    p[18]=0xF0;radio_w32(p+20,190);handle_radio_diagnostic(p,40);
    assert(stored==6 && saved_batch==3332029996LL && !strcmp(saved_event,"upload_ack_result"));
    assert(strstr(saved_message,"窗口校验结果（编号3）") && strstr(saved_message,"窗口=12") && strstr(saved_message,"190 ms"));
    assert(!strstr(saved_message,"累计观测轮询"));
    p[1]=RADIO_DIAG_SUBMIT;p[18]=0x10;handle_radio_diagnostic(p,40);
    assert(stored==7 && !strcmp(saved_event,"upload_ack_submit") && strstr(saved_message,"提交失败"));
    p[1]=RADIO_DIAG_MAC_RESULT;handle_radio_diagnostic(p,40);assert(stored==7);
    p[1]=RADIO_DIAG_POLL;handle_radio_diagnostic(p,40);assert(stored==7);
    handle_radio_diagnostic(p,39);assert(stored==7);
    puts("Actual backend diagnostics PASS: opt-in switch, reconnect handshake, coverage coalescing/reboot, failure metadata to logs, no success/poll DB flood, malformed rejection");
}
