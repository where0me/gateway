#ifdef NDEBUG
#undef NDEBUG
#endif
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include "config_diag.h"
#include "control_sync.h"
#include "report_dedup.h"

/* The runner appends the actual main.c functions, not a model of their logic. */
static uint64_t now = 1000;
static uint64_t mg_millis(void) { return now; }

static int g_uart_fd = 1, g_uart_mutex, g_channel_operation_mutex;
static int monitoring, known = 1, online_updates, writes, lookups;
static unsigned char sent[32];
static int g_mqtt_store_ready;
static void *g_db;

enum { CHANNEL_OPERATION_PENDING = 1 };
static struct {
    int phase;
    unsigned node_count, target_channel;
    struct { char mac[32]; } nodes[2];
} g_channel_operation;
static const char *node_mac = "9D:2C:EF:00:25:E1:80:00";
static void confirm_visible_node(const char *mac,uint16_t address,const char *reason)
{ assert(!strcmp(mac,node_mac) && address==0xFAAE && reason && *reason); }
static void pthread_mutex_lock(int *m) { assert(!*m); *m = 1; }
static void pthread_mutex_unlock(int *m) { assert(*m); *m = 0; }
static uint16_t read_le16(const unsigned char *p);
static void write_le16(unsigned char *p, uint16_t value);
static void write_le32(unsigned char *p, uint32_t value);
static void trace_config_uart(unsigned char cmd, const unsigned char *payload, unsigned char len);
static int build_monitor_config_payload(uint16_t,const MonitorConfig_t *,unsigned char[31],char[32]);
static int control_delivery(const char *mac,uint32_t hash,int operation)
{(void)hash;assert(mac && operation==1);return 1;}
static int send_monitor_config_scoped(uint16_t address, const MonitorConfig_t *cfg);
void handle_cmd_99_config_query(unsigned char *payload, unsigned char len);

static int lookup_mac_by_short(uint16_t address, char mac[24])
{
    lookups++;
    if (!known || address != 0xFAAE) return 0;
    strcpy(mac, node_mac); return 1;
}
static int get_device_monitoring(const char *mac, int *enabled)
{ assert(!strcmp(mac, node_mac)); *enabled = monitoring; return 1; }
static int device_is_authorized(const char *mac) { return !strcmp(mac, node_mac); }
static int update_device_online(const char *mac, const char *address)
{ assert(!strcmp(mac, node_mac) && !strcmp(address, "FAAE")); online_updates++; return 0; }
static int mqtt_store_policy(void *db, const char *mac, unsigned mask)
{ (void)db; (void)mac; (void)mask; return 0; }
static void gateway_log_event(const char *level, const char *source, const char *event,
                             const char *mac, int address, long long batch, const char *format, ...)
{ (void)level; (void)source; (void)event; (void)mac; (void)address; (void)batch; (void)format; }
static MonitorConfig_t load_current_monitor_config(void)
{
    MonitorConfig_t cfg = {0};
    cfg.auto_interval_sec = 300; cfg.poll_interval_sec = 5;
    cfg.vibration_rate_hz = 26667; cfg.digital_mic_rate_hz = 46875; cfg.analog_mic_rate_hz = 256000;
    cfg.vibration_window_ms = 500; cfg.digital_mic_window_ms = cfg.analog_mic_window_ms = 1000;
    cfg.raw_upload_every_cycles = 1; cfg.raw_upload_generation = 1; cfg.sensor_enable_mask = 7;
    return cfg;
}
static void send_hex_frame_to_uart(int fd, unsigned char cmd, unsigned char len, const unsigned char *p)
{
    assert(fd == 1 && cmd == CONTROL_CONFIG_SYNC_CMD && len == sizeof(sent) && g_uart_mutex);
    memcpy(sent, p, len); writes++;
}
static void query(unsigned char reason)
{
    unsigned char p[] = {0xAE, 0xFA, reason};
    trace_config_uart(GW_RSP_CONFIG_QUERY, p, sizeof(p));
    handle_cmd_99_config_query(p, sizeof(p));
}
static void reply(unsigned cmd, unsigned char status)
{
    unsigned char p[] = {0xAE, 0xFA, GW_CMD_MONITOR_CONFIG, status, 226};
    trace_config_uart((unsigned char)cmd, p, cmd == GW_RSP_AF_CONFIRM ? 5 : 4);
}
int main(void)
{
    g_config_diag_output = tmpfile(); assert(g_config_diag_output);
    config_diag_start(NULL, now);
    int previous_lookups = lookups;
    trace_config_uart(GW_RSP_CONFIG_QUERY, (const unsigned char *)"abc", 3);
    assert(lookups == previous_lookups); // Disabled instrumentation does no extra DB lookup.
    query(3); reply(GW_RSP_COMMAND_QUEUED, 0); reply(GW_RSP_AF_CONFIRM, 0xF0);
    MonitorConfig_t cfg=load_current_monitor_config();
    assert(writes==0 && send_monitor_config_scoped(0xFAAE,&cfg));
    assert(ftell(g_config_diag_output) == 0 && writes == 1);
    unsigned char without_diag[32]; memcpy(without_diag, sent, sizeof(sent));
    config_diag_start("0", now); assert(!g_config_diag_enabled);
    config_diag_start("true", now); assert(!g_config_diag_enabled);
    config_diag_start("1", now); now += 1234;
    query(3);
    assert(writes==1 && send_monitor_config_scoped(0xFAAE,&cfg));
    assert(writes == 2 && !memcmp(without_diag, sent, sizeof(sent)));
    assert(online_updates == 2);
    reply(GW_RSP_COMMAND_QUEUED, 0);
    reply(GW_RSP_COMMAND_QUEUED, 0x10);
    reply(GW_RSP_AF_CONFIRM, 0);
    reply(GW_RSP_AF_CONFIRM, 0xF0);
    reply(GW_RSP_AF_CONFIRM, 0xF1);
    reply(GW_RSP_AF_CONFIRM, 0xE9);
    reply(GW_RSP_AF_CONFIRM, 0xB7); // Unknown codes are preserved, not mislabelled as MAC expiry.
    monitoring = 1; query(1); assert(send_monitor_config_scoped(0xFAAE,&cfg) && sent[28] == 6);
    query(2);
    g_channel_operation.phase = CHANNEL_OPERATION_PENDING;
    g_channel_operation.node_count = 1; g_channel_operation.target_channel = 15;
    strcpy(g_channel_operation.nodes[0].mac, node_mac);
    query(3);
    assert(send_monitor_config_scoped(0xFAAE,&cfg) && sent[28] == 7 && sent[29] == 15);
    int before = writes;
    known = 0; query(3); assert(writes == before);
    known = 1; g_uart_fd = -1; query(3);
    assert(!send_monitor_config_scoped(0xFAAE,&cfg) && writes == before);
    unsigned char invalid[] = {0xAE, 0xFA};
    trace_config_uart(GW_RSP_CONFIG_QUERY, invalid, 2);
    handle_cmd_99_config_query(invalid, 2); assert(writes == before);
    trace_config_uart(GW_RSP_COMMAND_QUEUED, NULL, 0);
    trace_config_uart(GW_RSP_AF_CONFIRM, invalid, 2);
    config_diag_tx(now, invalid, 2, node_mac, 1);
    long length = ftell(g_config_diag_output);
    unsigned char other[] = {0xAE, 0xFA, GW_CMD_MONITOR_CONTROL, 0, 1};
    trace_config_uart(GW_RSP_AF_CONFIRM, other, sizeof(other));
    trace_config_uart(GW_RSP_MONITOR_STATE, other, sizeof(other));
    assert(ftell(g_config_diag_output) == length);
    rewind(g_config_diag_output);
    char output[32768]; size_t count = fread(output, 1, sizeof(output) - 1, g_config_diag_output);
    output[count] = 0;
    assert(strstr(output, "后端+1.234s") && strstr(output, node_mac));
    assert(strstr(output, "RX99") && strstr(output, "TX0C") && strstr(output, "SUBMIT8E") && strstr(output, "CONFIRM90"));
    assert(strstr(output, "待机查询") && strstr(output, "入网/恢复") && strstr(output, "上传后"));
    assert(strstr(output, "监测开关=开启") && strstr(output, "旧版查询未声明上传已结束"));
    assert(strstr(output, "状态=0x00（成功）") && strstr(output, "MAC事务过期"));
    assert(strstr(output, "状态=0xB7（其他状态") && strstr(output, "ZCL序号=226"));
    assert(strstr(output, "不代表节点已应用配置") && strstr(output, "DROP99"));
    assert(strstr(output, "未连接，不发送") && strstr(output, "RX异常") && strstr(output, "TX0C异常"));
    assert(!strstr(output, "TRANSPORT_KEY") && !strstr(output, "password"));
    assert(!g_uart_mutex && !g_channel_operation_mutex);
    fclose(g_config_diag_output);
    puts("Actual backend config path: opt-in logging, idle queries, TX byte-for-byte preservation, stack submit/confirm, malformed/unknown/offline cases PASS");
    return 0;
}
