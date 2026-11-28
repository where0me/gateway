#ifndef COMM_TEST_GATEWAY_H
#define COMM_TEST_GATEWAY_H
#include "comm_test_protocol.h"
static void handle_comm_test_report(const uint8_t *p,unsigned length)
{
    if(!p || length!=COMM_TEST_REPORT_LEN || p[2]!=COMM_TEST_VERSION)return;
    char address[8],mac[32]="unknown";
    unsigned short_addr=p[0]|((unsigned)p[1]<<8);
    snprintf(address,sizeof(address),"%04X",short_addr);
    (void)get_mac_by_short_addr(address,mac);
    printf("[串口通信测试] MAC=%s | 短地址=0x%04X | 编号=%lu | 协调器已收到节点测试请求 | RSSI=%d dBm | LQI=%u | 应用回复%s（提交状态=0x%02X，节点收到以节点PASS日志为准）\n",
        mac,short_addr,comm_test_token(p+2),(int)(int8_t)p[7],(unsigned)p[8],
        p[9]?"提交失败":"已提交协议栈",(unsigned)p[9]);
}
#endif
