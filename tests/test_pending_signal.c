#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint16_t short_addr;
    char mac_str[24];
    unsigned char mac_bin[8];
    int is_confirmed;
    int8_t rssi;
    uint16_t signal_token;
    int signal_log_requested;
    uint64_t signal_deadline_ms;
    time_t signal_updated_at;
    int signal_dirty;
    uint8_t lqi;
    char device_name[64];
} PendingDevice_t;
static PendingDevice_t g_pending_list[100];
static int g_pending_count, g_scan_state, g_pending_mutex, g_ws_mutex, g_uart_mutex;
static char g_ws_msg_buffer[512];
static int g_has_new_ws_msg, g_uart_fd = 3;
static uint64_t test_ms;
static unsigned sends;
static uint8_t last_request[4];
static int test_clock(int unused, struct timespec *value)
{
    (void)unused;
    value->tv_sec = (time_t)(test_ms / 1000U);
    value->tv_nsec = (long)(test_ms % 1000U) * 1000000L;
    return 0;
}
#define clock_gettime test_clock
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
static void send_hex_frame_to_uart(int fd, unsigned char cmd,
                                   unsigned char len, const unsigned char *data)
{
    assert(fd == 3 && cmd == 0x10 && len == 4);
    memcpy(last_request, data, 4);
    ++sends;
}
#include "pending_signal.h"

int main(void)
{
    g_pending_count = 3;
    for (int i = 0; i < 3; ++i) {
        g_pending_list[i].short_addr = (uint16_t)(0x1234 + i);
        snprintf(g_pending_list[i].mac_str, 24, "node-%d", i);
        strcpy(g_pending_list[i].device_name, "HY-Sensor");
        g_pending_list[i].rssi = -128;
    }
    g_pending_list[2].is_confirmed = 1;
    g_scan_state = 1;
    signal_refresh_cursor = 0;
    task_pending_signal();
    assert(sends == 1 && last_request[0] == 0x34);
    uint8_t reply[5] = { last_request[0], last_request[1], last_request[2], last_request[3], (uint8_t)-52 };
    reply[2]++;
    handle_pending_signal(reply, 5);
    assert(g_pending_list[0].rssi == -128); /* stale token rejected */
    reply[2]--;
    handle_pending_signal(reply, 4);
    assert(g_pending_list[0].rssi == -128); /* malformed reply rejected */
    handle_pending_signal(reply, 5);
    assert(g_pending_list[0].rssi == -52 && g_pending_list[0].signal_dirty);
    test_ms = 149;
    task_pending_signal();
    assert(sends == 1 && strstr(g_ws_msg_buffer, "\"rssi\":-52"));
    g_has_new_ws_msg = 0;
    test_ms = 150;
    task_pending_signal();
    assert(sends == 2 && last_request[0] == 0x35);
    g_pending_list[1].signal_updated_at = 123;
    test_ms = 4150;
    task_pending_signal();
    assert(sends == 2 && signal_refresh_cursor == -1);
    assert(strstr(g_ws_msg_buffer, "\"type\":\"scan_signal\""));
    assert(strstr(g_ws_msg_buffer, "\"reachable\":false"));
    assert(strstr(g_ws_msg_buffer, "\"timestamp\":123"));
    assert(strstr(g_ws_msg_buffer, "\"mac\":\"node-1\""));
    assert(g_pending_count == 3 && !g_pending_list[1].is_confirmed);
    reply[0] = last_request[0]; reply[1] = last_request[1];
    reply[2] = last_request[2]; reply[3] = last_request[3];
    handle_pending_signal(reply, 5);
    assert(g_pending_count == 3 && g_pending_list[1].rssi == -128); /* late reply rejected */
    g_has_new_ws_msg = 0;
    /* Repeated idle processing cannot resurrect an offline node or timestamp. */
    for (int i = 0; i < 10; ++i) {
        test_ms += 500;
        task_pending_signal();
        assert(!g_has_new_ws_msg && g_pending_list[1].signal_updated_at == 123);
    }
    /* A fresh token-matched reply after power restoration restores reachability. */
    signal_refresh_cursor = 1;
    task_pending_signal();
    assert(sends == 3);
    memcpy(reply, last_request, 4);
    reply[4] = (uint8_t)-61;
    handle_pending_signal(reply, 5);
    task_pending_signal();
    assert(strstr(g_ws_msg_buffer, "\"reachable\":true"));
    assert(strstr(g_ws_msg_buffer, "\"rssi\":-61"));
    g_has_new_ws_msg = 0;
    signal_refresh_cursor = 0;
    test_ms += 150;
    task_pending_signal();
    g_pending_list[0].is_confirmed = 1;
    memcpy(reply, last_request, 4);
    handle_pending_signal(reply, 5);
    assert(!g_pending_list[0].signal_dirty); /* authorization excludes refresh */
    g_scan_state = 2;
    task_pending_signal();
    assert(signal_refresh_cursor == -1 && !g_pending_list[0].signal_deadline_ms);
    handle_pending_signal(reply, 5);
    assert(!g_has_new_ws_msg && sends == 4);
    puts("Pending RSSI tests passed: pacing, tokens, timeout, authorization and scan stop.");
    return 0;
}
