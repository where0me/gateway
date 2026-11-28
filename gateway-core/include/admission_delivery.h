#ifndef ADMISSION_DELIVERY_H
#define ADMISSION_DELIVERY_H

#define ADMISSION_AF_WAIT_MS 12000U
#define ADMISSION_APPLY_WAIT_MS 5000U
#define ADMISSION_RETRY_DELAY_MS 2000U

enum AdmissionDeliveryPhase {
    ADMISSION_WAIT_AF, ADMISSION_WAIT_APPLIED, ADMISSION_RETRY
};

static struct AdmissionDelivery {
    char mac[24];
    uint32_t token;
    uint16_t address;
    uint64_t deadline;
    enum AdmissionDeliveryPhase phase;
} admission_deliveries[64];
static pthread_mutex_t admission_delivery_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t admission_next_token;
static int admission_grant_supported = -1;

static void admission_delivery_capabilities(unsigned char capabilities)
{
    pthread_mutex_lock(&admission_delivery_mutex);
    admission_grant_supported = (capabilities & GW_CAP_ADMISSION_GRANT) != 0;
    pthread_mutex_unlock(&admission_delivery_mutex);
}

/* Serialize the last pending-state check and send with application confirmation. */
static int admission_delivery_apply(const char *mac)
{
    pthread_mutex_lock(&admission_delivery_mutex);
    int applied = device_admission_requested(mac) ? authorize_device(mac) : 0;
    pthread_mutex_unlock(&admission_delivery_mutex);
    return applied;
}

static void send_admission_grant(const char *mac, int new_request)
{
    uint16_t address;
    pthread_mutex_lock(&admission_delivery_mutex);
    if (admission_grant_supported < 0 || !device_admission_requested(mac) ||
        !get_device_short_addr(mac, &address)) {
        pthread_mutex_unlock(&admission_delivery_mutex);
        return;
    }
    if (!admission_grant_supported) {
        (void)send_monitor_control(address, MONITOR_OP_AUTHORIZE, 0);
        pthread_mutex_unlock(&admission_delivery_mutex);
        return;
    }

    uint64_t now = signal_now_ms();
    struct AdmissionDelivery *entry = NULL, *available = NULL;
    for (size_t i = 0; i < sizeof(admission_deliveries) / sizeof(admission_deliveries[0]); ++i) {
        struct AdmissionDelivery *candidate = &admission_deliveries[i];
        if (!strcmp(candidate->mac, mac)) {
            entry = candidate;
            break;
        }
        if (candidate->deadline <= now && (!available || candidate->deadline < available->deadline))
            available = candidate;
    }
    if (entry && !new_request && entry->deadline > now) {
        pthread_mutex_unlock(&admission_delivery_mutex);
        return;
    }
    if (entry && !new_request && entry->phase != ADMISSION_RETRY) {
        printf("[授权投递] MAC=%s | 授权编号=%u | 等待%s超时，准备重试\n",
               mac, entry->token, entry->phase == ADMISSION_WAIT_AF ? "协调器投递结果" : "节点授权确认");
    }
    if (!entry) entry = available;
    if (!entry) {
        pthread_mutex_unlock(&admission_delivery_mutex);
        return;
    }

    if (!admission_next_token && !mg_random(&admission_next_token, sizeof(admission_next_token)))
        admission_next_token = (uint32_t)now;
    if (!++admission_next_token) ++admission_next_token;
    *entry = (struct AdmissionDelivery){.token = admission_next_token, .address = address};
    snprintf(entry->mac, sizeof(entry->mac), "%s", mac);

    unsigned char payload[6];
    write_le16(payload, address);
    write_le32(payload + 2, entry->token);
    pthread_mutex_lock(&g_uart_mutex);
    int sent = g_uart_fd >= 0 &&
        send_hex_frame_to_uart_checked(g_uart_fd, GW_CMD_ADMISSION_GRANT, sizeof(payload), payload) == 0;
    pthread_mutex_unlock(&g_uart_mutex);
    entry->phase = sent ? ADMISSION_WAIT_AF : ADMISSION_RETRY;
    entry->deadline = signal_now_ms() + (sent ? ADMISSION_AF_WAIT_MS : ADMISSION_RETRY_DELAY_MS);
    if (sent)
        printf("[授权投递] MAC=%s | 短地址=0x%04X | 授权编号=%u | 已提交串口\n",
               mac, address, entry->token);
    pthread_mutex_unlock(&admission_delivery_mutex);
}

static void admission_delivery_result(const unsigned char *p)
{
    uint16_t address = read_le16(p);
    uint32_t token = read_le32(p + 5);
    unsigned char status = p[3], stage = p[9];
    if (!token || (stage != GW_ADMISSION_AF_RESULT && stage != GW_ADMISSION_SUBMIT_FAILED)) return;

    pthread_mutex_lock(&admission_delivery_mutex);
    for (size_t i = 0; i < sizeof(admission_deliveries) / sizeof(admission_deliveries[0]); ++i) {
        struct AdmissionDelivery *entry = &admission_deliveries[i];
        if (entry->token != token || entry->address != address) continue;
        if (device_is_authorized(entry->mac)) {
            if (status)
                printf("[授权投递] MAC=%s | 授权编号=%u | 节点已确认授权，忽略迟到的失败 0x%02X\n",
                       entry->mac, token, status);
        } else if (entry->phase == ADMISSION_WAIT_AF && device_admission_requested(entry->mac)) {
            entry->phase = status ? ADMISSION_RETRY : ADMISSION_WAIT_APPLIED;
            entry->deadline = signal_now_ms() + (status ? ADMISSION_RETRY_DELAY_MS : ADMISSION_APPLY_WAIT_MS);
            if (status) {
                const char *source = stage == GW_ADMISSION_AF_RESULT ? "无线投递" : "协调器提交";
                printf("[授权投递] MAC=%s | 授权编号=%u | %s失败 0x%02X，仍待授权，将重试\n",
                       entry->mac, token, source, status);
                gateway_log_event("WARN", "coordinator", "admission_delivery_retry", entry->mac, address, -1,
                                  "授权编号 %u，%s失败 0x%02X，仍待授权，将重试", token, source, status);
            } else {
                printf("[授权投递] MAC=%s | 授权编号=%u | 无线投递成功，等待节点授权确认\n",
                       entry->mac, token);
            }
        }
        break;
    }
    pthread_mutex_unlock(&admission_delivery_mutex);
}

#endif
