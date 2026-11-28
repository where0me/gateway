#ifndef PENDING_SIGNAL_H
#define PENDING_SIGNAL_H
#ifndef GATEWAY_WS_EVENT_READY
#define GATEWAY_WS_EVENT_READY() (g_has_new_ws_msg=1)
#endif
/* Included once after the pending-device, UART and WebSocket state in main.c. */
static uint16_t signal_sequence;
static int signal_refresh_cursor = -1;
static int signal_batch_manual;
static uint64_t signal_next_send_ms;

static uint64_t signal_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* g_pending_mutex held. Only call for a token-matched live probe reply.
 * Neighbor snapshots and model replies must not publish cached signals. */
static void publish_pending_signal(PendingDevice_t *device, int rssi)
{
    device->rssi = (int8_t)rssi;
    device->signal_updated_at = time(NULL);
    device->signal_dirty = 1;
}

static void task_pending_signal(void)
{
    uint64_t now = signal_now_ms();
    uint8_t request[4];
    int send_probe = 0;
    pthread_mutex_lock(&g_pending_mutex);
    if (g_scan_state != 1) {
        signal_refresh_cursor = -1;
        for (int i = 0; i < g_pending_count; ++i)
        {
            g_pending_list[i].signal_deadline_ms = 0;
            g_pending_list[i].signal_dirty = 0;
        }
        pthread_mutex_unlock(&g_pending_mutex);
        return;
    }
    for (int i = 0; i < g_pending_count; ++i) {
        PendingDevice_t *d = &g_pending_list[i];
        if (d->signal_deadline_ms && now >= d->signal_deadline_ms) {
            if (d->is_confirmed) {
                d->signal_deadline_ms = 0;
            } else {
                /* Keep the expired deadline until the removal event can be queued. */
                pthread_mutex_lock(&g_ws_mutex);
                if (!g_has_new_ws_msg) {
                    if (d->signal_log_requested)
                        printf("[信号刷新] MAC=%s | 探测超时，暂不显示，后端记录保留\n", d->mac_str);
                    snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer),
                        "{\"type\":\"scan_signal\",\"mac\":\"%s\",\"model_name\":\"%s\","
                        "\"rssi\":null,\"lqi\":%u,\"reachable\":false,\"timestamp\":%lld}",
                        d->mac_str, d->device_name, d->lqi,
                        (long long)d->signal_updated_at);
                    GATEWAY_WS_EVENT_READY();
                    d->signal_deadline_ms = 0;
                    d->signal_log_requested = 0;
                    d->signal_dirty = 0;
                    d->rssi = -128;
                }
                pthread_mutex_unlock(&g_ws_mutex);
            }
            break;
        }
    }
    if (signal_refresh_cursor >= 0 && now >= signal_next_send_ms) {
        while (signal_refresh_cursor < g_pending_count) {
            PendingDevice_t *d = &g_pending_list[signal_refresh_cursor++];
            if (d->is_confirmed || d->signal_deadline_ms ||
                strcmp(d->device_name, "Waiting...") == 0) continue;
            if (++signal_sequence == 0) ++signal_sequence;
            d->signal_token = signal_sequence;
            d->signal_log_requested = signal_batch_manual;
            d->signal_deadline_ms = now + 4000U;
            request[0] = (uint8_t)d->short_addr;
            request[1] = (uint8_t)(d->short_addr >> 8);
            request[2] = (uint8_t)signal_sequence;
            request[3] = (uint8_t)(signal_sequence >> 8);
            send_probe = 1;
            signal_next_send_ms = now + 150U;
            break;
        }
        if (signal_refresh_cursor >= g_pending_count) signal_refresh_cursor = -1;
    }
    pthread_mutex_lock(&g_ws_mutex);
    if (!g_has_new_ws_msg) for (int i = 0; i < g_pending_count; ++i) {
        PendingDevice_t *d = &g_pending_list[i];
        if (!d->is_confirmed && d->signal_dirty) {
            char value[16];
            if (d->rssi == -128) strcpy(value, "null");
            else snprintf(value, sizeof(value), "%d", d->rssi);
            snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer),
                "{\"type\":\"scan_signal\",\"mac\":\"%s\",\"model_name\":\"%s\","
                "\"rssi\":%s,\"lqi\":%u,\"reachable\":true,\"timestamp\":%lld}",
                d->mac_str, d->device_name, value, d->lqi,
                (long long)d->signal_updated_at);
            d->signal_dirty = 0;
            GATEWAY_WS_EVENT_READY();
            break;
        }
    }
    pthread_mutex_unlock(&g_ws_mutex);
    pthread_mutex_unlock(&g_pending_mutex);
    if (send_probe) {
        pthread_mutex_lock(&g_uart_mutex);
        if (g_uart_fd >= 0) send_hex_frame_to_uart(g_uart_fd, 0x10U, sizeof(request), request);
        pthread_mutex_unlock(&g_uart_mutex);
    }
}

static void handle_pending_signal(const uint8_t *payload, unsigned len)
{
    if (len != 5U) return;
    uint16_t address = payload[0] | ((uint16_t)payload[1] << 8);
    uint16_t token = payload[2] | ((uint16_t)payload[3] << 8);
    pthread_mutex_lock(&g_pending_mutex);
    if (g_scan_state == 1) for (int i = 0; i < g_pending_count; ++i) {
        PendingDevice_t *d = &g_pending_list[i];
        if (!d->is_confirmed && d->short_addr == address &&
            d->signal_deadline_ms && d->signal_token == token &&
            signal_now_ms() < d->signal_deadline_ms) {
            d->signal_deadline_ms = 0;
            if (d->signal_log_requested) {
                if ((int8_t)payload[4] == -128)
                    printf("[信号刷新] MAC=%s | 本次未获取有效信号强度\n", d->mac_str);
                else
                    printf("[信号刷新] MAC=%s | 当前信号强度=%d dBm\n", d->mac_str, (int8_t)payload[4]);
                d->signal_log_requested = 0;
            }
            publish_pending_signal(d, (int8_t)payload[4]);
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_mutex);
}
#endif
