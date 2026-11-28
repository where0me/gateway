#ifndef GATEWAY_CONFIG_DIAG_H
#define GATEWAY_CONFIG_DIAG_H

/* Opt-in stdout-only tracing. Never sends frames, retries or mutates device state. */
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "monitor_protocol.h"

static int g_config_diag_enabled;
static uint64_t g_config_diag_started_ms;
static FILE *g_config_diag_output;

static void config_diag_start(const char *value, uint64_t now_ms)
{
    g_config_diag_enabled = value && strcmp(value, "1") == 0;
    g_config_diag_started_ms = now_ms;
}

static void config_diag_print(uint64_t now_ms, const char *format, ...)
{
    if (!g_config_diag_enabled) return;
    char message[1024], stamp[32];
    struct timespec wall = {0};
    struct tm local = {0};
    (void)timespec_get(&wall, TIME_UTC);
#ifdef _WIN32
    (void)localtime_s(&local, &wall.tv_sec);
#else
    (void)localtime_r(&wall.tv_sec, &local);
#endif
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    uint64_t elapsed = now_ms >= g_config_diag_started_ms ? now_ms - g_config_diag_started_ms : 0;
    FILE *output = g_config_diag_output ? g_config_diag_output : stdout;
    /* One stdio write per line avoids mixing UART-thread and HTTP-thread traces. */
    fprintf(output, "[配置诊断 %s.%03ld 后端+%llu.%03llus] %s\n", stamp,
            wall.tv_nsec / 1000000L, (unsigned long long)(elapsed / 1000U),
            (unsigned long long)(elapsed % 1000U), message);
    fflush(output);
}

static const char *config_diag_reason(unsigned reason)
{
    switch (reason) {
        case 1: return "入网/恢复";
        case 2: return "上传后";
        case 3: return "待机查询";
        default: return "未知原因";
    }
}

static const char *config_diag_status(unsigned status)
{
    switch (status) {
        case 0: return "成功";
        case 0xF0: return "MAC事务过期（非明确的APS ACK超时）";
        case 0xF1: return "MAC事务溢出";
        case 0xE9: return "MAC未收到ACK";
        default: return "其他状态，以协议栈原码为准";
    }
}

static unsigned config_diag_u16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static uint32_t config_diag_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void config_diag_frame(uint64_t now_ms, unsigned cmd,
                              const unsigned char *p, unsigned len, const char *mac)
{
    if (!g_config_diag_enabled) return;
    if (cmd != GW_RSP_CONFIG_QUERY && cmd != GW_RSP_COMMAND_QUEUED && cmd != GW_RSP_AF_CONFIRM) return;
    unsigned minimum = cmd == GW_RSP_CONFIG_QUERY ? 3U : cmd == GW_RSP_AF_CONFIRM ? 5U : 4U;
    if (!p || len < minimum) {
        config_diag_print(now_ms, "RX异常 | 串口命令=0x%02X | 长度=%u | 至少需要=%u字节", cmd, len, minimum);
        return;
    }
    const char *identity = mac && mac[0] ? mac : "未登记/当前映射未知";
    unsigned address = config_diag_u16(p);
    if (cmd == GW_RSP_CONFIG_QUERY) {
        config_diag_print(now_ms, "RX99 查询到达后端 | MAC=%s | 短地址=0x%04X | 原因=0x%02X（%s）| 长度=%u",
                          identity, address, p[2], config_diag_reason(p[2]), len);
    } else if (p[2] == GW_CMD_MONITOR_CONFIG) {
        if (cmd == GW_RSP_COMMAND_QUEUED)
            config_diag_print(now_ms, "SUBMIT8E 协调器协议栈提交结果 | MAC=%s | 短地址=0x%04X | 命令=0x0C | 状态=0x%02X（%s）| %s",
                              identity, address, p[3], config_diag_status(p[3]),
                              p[3] ? "提交被拒绝" : "仅接受发送请求，尚未完成无线投递");
        else
            config_diag_print(now_ms, "CONFIRM90 无线投递结果 | MAC=%s | 短地址=0x%04X | 命令=0x0C | 状态=0x%02X（%s）| ZCL序号=%u | %s",
                              identity, address, p[3], config_diag_status(p[3]), p[4],
                              p[3] ? "无线投递失败" : "无线投递确认成功，不代表节点已应用配置");
    }
}

static void config_diag_tx(uint64_t now_ms, const unsigned char *p,
                          unsigned len, const char *mac, int uart_available)
{
    if (!g_config_diag_enabled) return;
    if (!p || len != 31U) {
        config_diag_print(now_ms, "TX0C异常 | 配置载荷长度=%u（预期31）", len);
        return;
    }
    config_diag_print(now_ms,
        "TX0C 配置下发意图 | MAC=%s | 短地址=0x%04X | 周期=%us | 查询间隔=%us | 振动=%ums | 数字麦克风=%ums | 超声麦克风=%ums | 监测开关=%s | flags=0x%02X | sensor_mask=0x%02X | UART=%s",
        mac && mac[0] ? mac : "未登记/当前映射未知", config_diag_u16(p),
        (unsigned)config_diag_u32(p + 2), config_diag_u16(p + 6), config_diag_u16(p + 16),
        config_diag_u16(p + 18), config_diag_u16(p + 20),
        !(p[28] & 4U) ? "未携带" : (p[28] & 2U) ? "开启" : "关闭", p[28], p[30],
        uart_available ? "即将调用发送接口（不证明串口写入或协调器接收成功）" : "未连接，不发送");
}

#endif
