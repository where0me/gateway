#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static unsigned lookups;static char text[1024];
static int get_mac_by_short_addr(const char *address,char *mac)
{assert(!strcmp(address,"714C"));strcpy(mac,"01:02:03:04:05:06:07:08");lookups++;return 1;}
static int capture(const char *fmt,...)
{va_list ap;va_start(ap,fmt);int n=vsnprintf(text,sizeof(text),fmt,ap);va_end(ap);return n;}
#define printf capture
#include "comm_test_gateway.h"
#undef printf
int main(void)
{
    uint8_t p[10]={0x4c,0x71};comm_test_request(p+2,123456U);p[7]=(uint8_t)-47;p[8]=156;
    handle_comm_test_report(p,9);assert(!lookups);p[2]=2;handle_comm_test_report(p,10);assert(!lookups);
    p[2]=1;handle_comm_test_report(p,10);assert(lookups==1 && strstr(text,"123456") && strstr(text,"-47") && strstr(text,"156"));
    assert(strstr(text,"已收到节点测试请求") && strstr(text,"已提交协议栈"));
    p[9]=0x10;handle_comm_test_report(p,10);assert(lookups==2 && strstr(text,"提交失败"));
    puts("Gateway comm diagnostic PASS: validated version/length, exact token/metrics, truthful submit vs receipt; lookup only, no device state writes");
}
