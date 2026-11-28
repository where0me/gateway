#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#define MG_ENABLE_HTTP 1
#define MG_ENABLE_LOG 0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glob.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>     
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>  
#include <strings.h>
#include <time.h>
#include "mongoose.h"
#include "cJSON.h"
#include "uart_driver.h"
#include "db_manager.h"
#include "gateway_log.h"
#include "console_log.h"
#include "gateway_log_query.h"
#include <signal.h>
#include <fcntl.h>
#include "gateway_mqtt.h"
#include "gateway_mqtt_api.h"
static GatewayMqttApi g_mqtt_api;
extern sqlite3 *g_db;
static int g_mqtt_store_ready;
static void mqtt_note_batch(const char *mac,uint32_t batch,enum MqttBatchEvent event)
{
    if(g_mqtt_store_ready && mqtt_store_note(g_db,mac,batch,event)!=0) {
        fprintf(stderr,"[MQTT] 批次完成依据保存失败 | MAC=%s | 批次=%u；采集数据仍保留在本地\n",mac,batch);
        gateway_log_event("ERROR","backend","mqtt_batch_metadata_failed",mac,-1,batch,
                          "MQTT批次完成依据保存失败，保留源数据待核验");
    }
}
#include "monitor_protocol.h"
#include "config_diag.h"
#include "radio_diag.h"
#include "control_sync.h"
#include "gateway_exchange_wire.h"
#include "vibration_rx.h"
#include "report_dedup.h"
#include "monitor_report.h"
/* UART receive thread owns this cache. Idle telemetry is never deduplicated. */
static ReportDedup g_report_replays;

/* Some editors use non-POSIX host headers for IntelliSense. Linux normally
 * provides PATH_MAX through <limits.h>; retain a conventional safe fallback. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// 状态定义
typedef enum { STATE_SOF, STATE_CMD, STATE_LEN, STATE_DATA, STATE_BCC } ParserState_e;

// 解析器上下文结构体
typedef struct {
    ParserState_e state;           // 当前状态
    unsigned char rx_cmd;          // 当前指令
    unsigned char payload_len;     // 载荷长度
    unsigned char rx_payload[256]; // 载荷缓存
    int payload_idx;               // 载荷写入索引
    unsigned char bcc_calc;        // 实时计算的校验和
} UartParser_t;



// 1. 映射表（结构体类型的数组）：用于将 0x85 回复的短地址重新匹配回 MAC 地址
#define MAX_PENDING_DEVICES 100
#define REMOVE_RETRY_INTERVAL_SEC 3LL
#define SCAN_NEIGHBOR_POLL_MS 5000ULL
#define SCAN_NEIGHBOR_TIMEOUT_MS 2000ULL
#define SCAN_LEAVE_WAIT_MS 10000ULL
typedef struct {
    uint16_t short_addr;
    char mac_str[24];
    unsigned char mac_bin[8]; // 💡 新增：保存原始二进制MAC，踢人时直接发送
    int is_confirmed;         // 💡 新增：0: 待定, 1: 确认
    int8_t rssi;
    uint16_t signal_token;
    int signal_log_requested;
    uint64_t signal_deadline_ms;
    time_t signal_updated_at;
    int signal_dirty;
    uint8_t lqi;
    int leave_requested;
    int leave_completed;
    char device_name[64];
} PendingDevice_t;

PendingDevice_t g_pending_list[MAX_PENDING_DEVICES];//映射表
pthread_mutex_t g_pending_mutex = PTHREAD_MUTEX_INITIALIZER;//映射表锁
int g_pending_count = 0;//表内设备数量

/* Device Announce is only a candidate. A fresh coordinator NIB snapshot must
 * contain the same IEEE and short address before the node enters g_pending_list. */
typedef struct {
    unsigned char address[10];
} ScanAnnounce_t;
typedef struct {
    unsigned char mac[8];
    uint16_t short_addr;
    uint8_t lqi;
    int8_t rssi;
    uint8_t relationship;
} ScanNeighbor_t;
static ScanAnnounce_t g_scan_announces[MAX_PENDING_DEVICES];
static int g_scan_announce_count;
static ScanNeighbor_t g_scan_neighbors[MAX_PENDING_DEVICES];
static int g_scan_neighbor_count;
static uint8_t g_scan_neighbor_token;
static int g_scan_neighbor_awaiting;
static int g_scan_neighbor_processing;
static uint64_t g_scan_neighbor_next_ms;

// =======================================
// 2. 任务队列：先进先出 (FIFO) 链表，存放等待被询问名字的设备
typedef struct Node {
    uint16_t short_addr;   // 核心数据：设备的短地址
    struct Node* next;     // 指针：指向队伍里的下一个人
} ScanQueueNode_t;

ScanQueueNode_t* g_queue_head = NULL; //链表头
ScanQueueNode_t* g_queue_tail = NULL; //链表尾
pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER; //队列表锁
// =======================================

/*********************************************************************
 * 函数声明
 *********************************************************************/


void *uart_rx_thread_func(void *arg);//串口线程处理
static void gx_retry_tick(void);
void uart_parser_init(UartParser_t *parser);//解析器初始化
void uart_parse_byte(UartParser_t *parser, unsigned char c);//解析器（状态机）：用来拆解协调器发来的报文
void dispatch_uart_command(unsigned char cmd, unsigned char *payload, unsigned char len);//串口上行数据路由分发器

void handle_cmd_84_device_join(unsigned char *payload, unsigned char len);//0x84报文处理逻辑，在扫描中，设备初步入网后传来的设备地址报文，放入调度队列和映射表 
void handle_cmd_85_device_info(unsigned char *payload, unsigned char len);//0x85报文处理逻辑，在扫描中，设备初步入网后发来的设备信息，并将设备信息组装好，激活标志位
void handle_cmd_86_temperature(unsigned char *payload, unsigned char len);//0x86报文处理逻辑，设备正式入网后，上传的温度数据
void handle_cmd_87_magnetic(unsigned char *payload, unsigned char len);//0x87报文处理逻辑，设备正式入网后，磁力数据处理函数声明
void handle_cmd_88_89_sound(unsigned char *payload, unsigned char len, int is_digital);//0x88/0x89数字与超声麦克风声级
void handle_cmd_8a_network_info(unsigned char *payload, unsigned char len);//0x8A报文处理逻辑，保存协调器当前网络信息
void handle_cmd_9a_network_restore(unsigned char *payload, unsigned char len);
void handle_cmd_8b_vibration_chunk(unsigned char *payload, unsigned char len);
void handle_cmd_8c_vibration_summary(unsigned char *payload, unsigned char len);
void handle_cmd_8d_monitor_state(unsigned char *payload, unsigned char len);
void handle_cmd_99_config_query(unsigned char *payload, unsigned char len);
static void handle_control_status(const unsigned char *payload, unsigned len);
void enqueue_device(uint16_t short_addr);//入队列链表
int dequeue_device(uint16_t *out_short_addr);//出队列链表



static void ev_handler(struct mg_connection *c, int ev, void *ev_data);//后端与前端通信,并且下发命令
void route_http_requests(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_device_add(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_device_list(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_device_remove(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_device_rename(struct mg_connection *c, struct mg_http_message *hm);//节点重命名
static void api_handle_device_description(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_temp_history(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_sound_history(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_collection_history(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_system_settings_get(struct mg_connection *c);
void api_handle_channel_settings(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_system_time_sync(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_system_resources_get(struct mg_connection *c);
void api_handle_serial_devices_get(struct mg_connection *c);
void api_handle_serial_settings(struct mg_connection *c, struct mg_http_message *hm, int save_config);
void api_handle_monitor_settings(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_device_monitor(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_device_collect(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_monitor_all(struct mg_connection *c, struct mg_http_message *hm);
void api_handle_collect_all(struct mg_connection *c);
void api_handle_vibration_latest(struct mg_connection *c, struct mg_http_message *hm);
void task_broadcast_ws(struct mg_connection *c);
void task_channel_migration(void);
void task_periodic_maintenance(void);//周期维护任务（不负责判断节点离线）
void task_dispatch_queries(void);
void task_cleanup_pending(void);
static void handle_scan_neighbor_entry(const unsigned char *payload, unsigned char len);
static void handle_scan_neighbor_done(const unsigned char *payload, unsigned char len);
static void accept_verified_scan_node(const unsigned char *payload, uint8_t lqi, int8_t rssi);

static int normalize_custom_name(const char *input, char *output, size_t output_size);
static void publish_name_update(const char *mac,const char *model_name,const char *custom_name,const char *display_name);
static int load_gateway_config(void);
static int save_gateway_config(void);
static void request_network_info(void);
static MonitorConfig_t load_current_monitor_config(void);
static MonitorConfig_t load_monitor_config_for_mac(const char *mac);
static void manual_admission_tick(void);
static int cancel_pending_admissions(void);
static cJSON *parse_http_json(const struct mg_http_message *hm);
static int retry_removed_device_by_short(uint16_t short_addr,
                                         unsigned char trigger_cmd);
static void channel_migration_note_node_awake(uint16_t short_addr);
static void channel_migration_remove_node(const char *mac);
static void channel_migration_commit_if_ready(void);
static void persist_channel_operation_snapshot(void);
static void restore_channel_operation(void);



/*********************************************************************
 * 全局变量
 *********************************************************************/
#define SENSOR_HISTORY_RETENTION_SEC (12 * 60 * 60)     //存12个小时的数据
#define HISTORY_CLEANUP_PERIOD 60000                    //每60秒执行一次清理检查
#define ONLINE_STATUS_CHECK_PERIOD 1000UL                //每秒最多处理一个超时节点，保证WebSocket逐条送达
#define ONLINE_STATUS_GRACE_SEC 30ULL                    //两个采集周期之外的调度余量
#define MIN_VALID_SYSTEM_EPOCH 1704067200LL              //2024-01-01 00:00:00 UTC
#define MAX_VALID_SYSTEM_EPOCH 4102444800LL              //2100-01-01 00:00:00 UTC
#define GATEWAY_CONFIG_FILE "gateway_config.json"       //Linux网关本机运行配置，不纳入Git
#define GATEWAY_BAUD_RATE 921600
#define MAX_SERIAL_DEVICES 64
#define COORDINATOR_QUERY_PERIOD_MS 5000ULL
#define COORDINATOR_RESPONSE_TIMEOUT_MS 15000ULL

typedef struct {
    char path[256];
    char stable_path[PATH_MAX];
    char description[256];
    char kind[16];
    int accessible;
} SerialDevice_t;

static int enumerate_serial_devices(SerialDevice_t *devices, int max_devices);

// 全局文件描述符，用于保存打开的串口设备句柄
int g_uart_fd = -1;
pthread_mutex_t g_uart_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_radio_diag_enabled;
static void request_radio_diagnostics(void)
{
    unsigned char enabled=g_radio_diag_enabled?1U:0U;
    pthread_mutex_lock(&g_uart_mutex);
    if(g_uart_fd>=0)send_hex_frame_to_uart(g_uart_fd,RADIO_DIAG_CMD,1,&enabled);
    pthread_mutex_unlock(&g_uart_mutex);
}
char g_serial_path[256] = {0}; // 唯一配置来源：gateway_config.json 的 serial_path

// 协调器上报的实际网络信息，供系统设置页只读展示
pthread_mutex_t g_network_info_mutex = PTHREAD_MUTEX_INITIALIZER;
uint16_t g_network_pan_id = 0;
unsigned char g_network_channel = 0;
unsigned char g_network_device_state = 0;
int g_has_network_info = 0;
unsigned char g_network_startup_source = GW_NETWORK_SOURCE_UNKNOWN;
unsigned char g_network_restore_result = 0xFFU;
unsigned char g_network_restore_status = 0xFFU;
unsigned char g_network_startup_status = 0xFFU;
unsigned char g_network_state = GW_NETWORK_STATE_NOT_READY;
uint64_t g_network_last_seen_monotonic_ms = 0U;
uint64_t g_network_last_seen_epoch_ms = 0U;

static uint64_t coordinator_clock_ms(clockid_t clock_id)
{
    struct timespec now;
    if (clock_gettime(clock_id, &now) != 0) return 0U;
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static const char *network_state_text(unsigned char state)
{
    switch (state)
    {
        case GW_NETWORK_STATE_NOT_READY: return "not_ready";
        case GW_NETWORK_STATE_RESTORING: return "restoring";
        case GW_NETWORK_STATE_SCANNING: return "scanning";
        case GW_NETWORK_STATE_FORMING: return "forming";
        case GW_NETWORK_STATE_READY: return "ready";
        case GW_NETWORK_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

/* 信道切换是异步操作：0x96 只表示请求是否被协调器受理，最终结果必须
 * 由后续 0x8A 网络信息中的实际信道确认。该状态同时供 WebSocket 和
 * /api/system/settings 轮询使用，避免前端把“已受理”误报为“已成功”。 */
typedef enum {
    CHANNEL_OPERATION_IDLE = 0,
    CHANNEL_OPERATION_PENDING,
    CHANNEL_OPERATION_SUCCESS,
    CHANNEL_OPERATION_FAILED
} ChannelOperationPhase_t;

#define CHANNEL_MIGRATION_MAX_NODES 100U
#define CHANNEL_NODE_WAITING  0U
#define CHANNEL_NODE_SENT     1U
#define CHANNEL_NODE_PREPARED 2U

typedef struct {
    char mac[32];
    uint16_t short_addr;
    unsigned char state;
} ChannelMigrationNode_t;

typedef struct {
    ChannelOperationPhase_t phase;
    unsigned char strategy;
    unsigned char target_channel;
    unsigned char current_channel;
    unsigned char coordinator_status;
    unsigned char waiting_for_nodes;
    unsigned char commit_sent;
    unsigned char node_count;
    unsigned char prepared_count;
    unsigned long next_query_ms;
    unsigned long deadline_ms;
    char message[160];
    ChannelMigrationNode_t nodes[CHANNEL_MIGRATION_MAX_NODES];
} ChannelOperation_t;

static ChannelOperation_t g_channel_operation = {0};
static pthread_mutex_t g_channel_operation_mutex = PTHREAD_MUTEX_INITIALIZER;

/* usleep() is obsolete in POSIX.1-2008 and may be hidden by modern feature
 * macros.  Keep the existing short delays with the supported nanosleep API. */
static void sleep_microseconds(unsigned int microseconds)
{
    struct timespec remaining = {
        .tv_sec = (time_t)(microseconds / 1000000U),
        .tv_nsec = (long)(microseconds % 1000000U) * 1000L
    };
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR)
    {
    }
}

// 协调器最近一次真实 PermitJoin 状态：0 关闭，1 开放。关网不清退设备。
int g_scan_state = 0;

// ======== 用于 WebSocket 线程间通信的共享变量 ========
pthread_mutex_t g_ws_mutex = PTHREAD_MUTEX_INITIALIZER; // 互斥锁
#include "ws_event_queue.h"
char g_ws_msg_buffer[8192] = {0}; // Producer scratch space, copied into the FIFO under the mutex.
int g_has_new_ws_msg = 0;        // 标志位：1代表有新设备数据
static WsEventQueue g_ws_events;
#define GATEWAY_WS_EVENT_READY() queue_ws_event_locked()
static void queue_ws_event_locked(void)
{
    (void)ws_event_push(&g_ws_events,g_ws_msg_buffer);
    g_has_new_ws_msg=1;
}
#include "pending_signal.h"
#include "gateway_join.h"
#include "admission_probe.h"
#include "gateway_admission.h"
// =======================================================



static void handle_scan_neighbor_entry(const unsigned char *payload, unsigned char len)
{
    if (len != 14U) return;
    pthread_mutex_lock(&g_pending_mutex);
    if ((g_scan_state == 1 || g_scan_state == 2) && g_scan_neighbor_awaiting &&
        payload[0] == g_scan_neighbor_token &&
        g_scan_neighbor_count < MAX_PENDING_DEVICES &&
        (payload[13] == 0x01U || payload[13] == 0x05U))
    {
        ScanNeighbor_t *entry = &g_scan_neighbors[g_scan_neighbor_count++];
        memcpy(entry->mac, payload + 1U, 8U);
        entry->short_addr = (uint16_t)payload[9] | ((uint16_t)payload[10] << 8U);
        entry->lqi = payload[11];
        entry->rssi = (int8_t)payload[12];
        entry->relationship = payload[13];
    }
    pthread_mutex_unlock(&g_pending_mutex);
}

static void handle_scan_neighbor_done(const unsigned char *payload, unsigned char len)
{
    if (len != 3U) return;
    pthread_mutex_lock(&g_pending_mutex);
    if ((g_scan_state != 1 && g_scan_state != 2) || !g_scan_neighbor_awaiting ||
        payload[0] != g_scan_neighbor_token)
    {
        pthread_mutex_unlock(&g_pending_mutex);
        return;
    }
    g_scan_neighbor_awaiting = 0;
    g_scan_neighbor_next_ms = signal_now_ms() + SCAN_NEIGHBOR_POLL_MS;
    if (payload[2] != 0U || payload[1] != (uint8_t)g_scan_neighbor_count)
    {
        g_scan_neighbor_count = 0;
        pthread_mutex_unlock(&g_pending_mutex);
        return;
    }
    g_scan_neighbor_processing = 1;
    for (int k = 0; k < g_pending_count; ++k)
    {
        PendingDevice_t *device = &g_pending_list[k];
        if (device->is_confirmed) continue;
        for (int n = 0; n < g_scan_neighbor_count; ++n)
        {
            const ScanNeighbor_t *neighbor = &g_scan_neighbors[n];
            if (memcmp(device->mac_bin, neighbor->mac, 8U) != 0 ||
                device->short_addr != neighbor->short_addr) continue;
            device->lqi = neighbor->lqi;
            /* Neighbor entries may outlive a powered-off node. Only a token-
             * matched live probe may publish RSSI/reachability or a new timestamp. */
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_mutex);

    for (;;)
    {
        unsigned char address[10];
        uint8_t lqi = 0U;
        int8_t rssi = -128;
        int found = 0;
        pthread_mutex_lock(&g_pending_mutex);
        if (g_scan_state == 1 || g_scan_state == 2)
        {
            for (int a = 0; a < g_scan_announce_count && !found; ++a)
            {
                for (int n = 0; n < g_scan_neighbor_count; ++n)
                {
                    const ScanNeighbor_t *neighbor = &g_scan_neighbors[n];
                    const ScanAnnounce_t *announce = &g_scan_announces[a];
                    if (memcmp(announce->address, neighbor->mac, 8U) == 0 &&
                        ((uint16_t)announce->address[8] |
                         ((uint16_t)announce->address[9] << 8U)) == neighbor->short_addr)
                    {
                        memcpy(address, announce->address, sizeof(address));
                        lqi = neighbor->lqi;
                        rssi = neighbor->rssi;
                        if (a + 1 < g_scan_announce_count)
                            memmove(&g_scan_announces[a], &g_scan_announces[a + 1],
                                    (size_t)(g_scan_announce_count - a - 1) *
                                    sizeof(g_scan_announces[0]));
                        g_scan_announce_count--;
                        found = 1;
                        break;
                    }
                }
            }
        }
        pthread_mutex_unlock(&g_pending_mutex);
        if (!found) break;
        accept_verified_scan_node(address, lqi, rssi);
    }
    pthread_mutex_lock(&g_pending_mutex);
    g_scan_neighbor_processing = 0;
    pthread_mutex_unlock(&g_pending_mutex);
}

#define VIBRATION_SAMPLES_PER_PACKET 8U
#define VIBRATION_MAX_SAMPLE_COUNT 40100U
#define VIBRATION_ASSEMBLY_SLOTS 8U

typedef struct
{
    int active;
    uint16_t short_addr;
    uint32_t batch_id;
    uint16_t packet_count;
    uint16_t received_packets;
    uint32_t received_samples;
    uint32_t sample_rate_hz;
    uint32_t sample_count;
    int summary_received;
    unsigned char *packet_bitmap;
    int16_t *samples;
    int32_t features[3][5];
    time_t last_update;
} VibrationAssembly_t;

static VibrationAssembly_t g_vibration_assemblies[VIBRATION_ASSEMBLY_SLOTS];
static pthread_mutex_t g_vibration_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value & 0xFFU);
    p[1] = (unsigned char)((value >> 8) & 0xFFU);
}

static void write_le32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value & 0xFFU);
    p[1] = (unsigned char)((value >> 8) & 0xFFU);
    p[2] = (unsigned char)((value >> 16) & 0xFFU);
    p[3] = (unsigned char)((value >> 24) & 0xFFU);
}

static const char *monitor_state_name(unsigned char state)
{
    switch (state)
    {
        case MONITOR_STATE_WAITING: return "waiting";
        case MONITOR_STATE_CAPTURING: return "capturing";
        case MONITOR_STATE_UPLOADING: return "uploading";
        case MONITOR_STATE_COMPLETE: return "complete";
        case MONITOR_STATE_ERROR: return "error";
        default: return "idle";
    }
}

static const char *downlink_command_description(unsigned char cmd)
{
    switch (cmd)
    {
        case GW_CMD_MONITOR_CONTROL: return "监测控制：开启/关闭在线监测或立即采集";
        case GW_CMD_MONITOR_CONFIG: return "采样配置：下发振动及麦克风采样参数";
        case GX_CONTROL: return "上传收尾：休眠决策或采样配置";
        case GW_CMD_CHANNEL_CONFIG: return "信道配置：设置自动策略或执行全网信道迁移";
        case 0x02U: return "节点移除：要求指定终端退出Zigbee网络";
        case 0x04U: return "设备信息查询：读取终端名称和型号";
        default: return "未知下行命令";
    }
}

static const char *zigbee_channel_status_description(unsigned char status)
{
    switch (status)
    {
        case 0x00U: return "请求已受理";
        case 0x01U: return "通用失败";
        case 0x02U: return "参数无效";
        case 0x10U: return "内存不足";
        case 0x11U: return "发送缓冲区已满";
        case 0x12U: return "协调器固件未启用网络更新功能";
        case 0xB7U: return "未收到目标节点确认";
        case 0xCDU: return "没有到目标节点的路由";
        default: return "未识别的Z-Stack状态";
    }
}

static const char *channel_operation_phase_text(ChannelOperationPhase_t phase)
{
    switch (phase)
    {
        case CHANNEL_OPERATION_PENDING: return "pending";
        case CHANNEL_OPERATION_SUCCESS: return "success";
        case CHANNEL_OPERATION_FAILED: return "failed";
        default: return "idle";
    }
}

static void publish_channel_operation_event(ChannelOperationPhase_t phase,
                                            unsigned char target_channel,
                                            unsigned char current_channel,
                                            unsigned char coordinator_status,
                                            unsigned char prepared_count,
                                            unsigned char node_count,
                                            const char *message)
{
    pthread_mutex_lock(&g_ws_mutex);
    snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer),
             "{\"type\":\"channel_migration\",\"state\":\"%s\","
             "\"target_channel\":%u,\"current_channel\":%u,"
             "\"coordinator_status\":%u,\"prepared_nodes\":%u,"
             "\"total_nodes\":%u,\"message\":\"%s\"}",
             channel_operation_phase_text(phase), target_channel,
             current_channel, coordinator_status, prepared_count,
             node_count, message ? message : "");
    queue_ws_event_locked();
    pthread_mutex_unlock(&g_ws_mutex);
}

static void begin_channel_operation(unsigned char strategy,
                                    unsigned char target_channel,
                                    unsigned int migration_delay_sec,
                                    const DeviceRecord_t *records,
                                    unsigned char node_count,
                                    int wait_for_nodes)
{
    unsigned long now = (unsigned long)mg_millis();
    pthread_mutex_lock(&g_channel_operation_mutex);
    g_channel_operation.phase = CHANNEL_OPERATION_PENDING;
    g_channel_operation.strategy = strategy;
    g_channel_operation.target_channel = target_channel;
    g_channel_operation.current_channel = 0U;
    g_channel_operation.coordinator_status = 0xFFU;
    g_channel_operation.waiting_for_nodes = wait_for_nodes ? 1U : 0U;
    g_channel_operation.commit_sent = wait_for_nodes ? 0U : 1U;
    g_channel_operation.node_count = node_count;
    g_channel_operation.prepared_count = 0U;
    g_channel_operation.next_query_ms = wait_for_nodes ? 0U : now + 500U;
    g_channel_operation.deadline_ms = wait_for_nodes ? 0U : now +
        ((unsigned long)migration_delay_sec + 15UL) * 1000UL;
    snprintf(g_channel_operation.message, sizeof(g_channel_operation.message),
             "%s", wait_for_nodes
                ? "等待节点唤醒并接收信道迁移通知"
                : "协调器正在执行信道操作");
    for (unsigned char i = 0U; i < node_count; ++i)
    {
        unsigned int short_addr = 0U;
        snprintf(g_channel_operation.nodes[i].mac,
                 sizeof(g_channel_operation.nodes[i].mac), "%s", records[i].mac);
        if (sscanf(records[i].short_addr, "%x", &short_addr) == 1)
            g_channel_operation.nodes[i].short_addr = (uint16_t)short_addr;
        g_channel_operation.nodes[i].state = CHANNEL_NODE_WAITING;
    }
    pthread_mutex_unlock(&g_channel_operation_mutex);
    persist_channel_operation_snapshot();
    publish_channel_operation_event(CHANNEL_OPERATION_PENDING, target_channel,
                                    0U, 0xFFU, 0U, node_count,
                                    wait_for_nodes
                                        ? "等待节点唤醒并接收信道迁移通知"
                                        : "协调器正在执行信道操作");
}

static void finish_channel_operation(ChannelOperationPhase_t phase,
                                     unsigned char current_channel,
                                     unsigned char coordinator_status,
                                     const char *message)
{
    unsigned char target_channel;
    unsigned char strategy;
    pthread_mutex_lock(&g_channel_operation_mutex);
    if (g_channel_operation.phase != CHANNEL_OPERATION_PENDING)
    {
        pthread_mutex_unlock(&g_channel_operation_mutex);
        return;
    }
    target_channel = g_channel_operation.target_channel;
    strategy = g_channel_operation.strategy;
    g_channel_operation.phase = phase;
    g_channel_operation.current_channel = current_channel;
    g_channel_operation.coordinator_status = coordinator_status;
    snprintf(g_channel_operation.message, sizeof(g_channel_operation.message),
             "%s", message ? message : "");
    unsigned char prepared_count = g_channel_operation.prepared_count;
    unsigned char node_count = g_channel_operation.node_count;
    pthread_mutex_unlock(&g_channel_operation_mutex);
    persist_channel_operation_snapshot();

    /* 手动切换无论成功或失败，都以协调器实际信道纠正持久化策略。 */
    if (strategy == 1U && current_channel >= 11U && current_channel <= 26U)
        (void)save_zigbee_channel_config("manual", current_channel);
    publish_channel_operation_event(phase, target_channel, current_channel,
                                    coordinator_status, prepared_count,
                                    node_count, message);
}

/* All listed terminals have already received the same network-update ID on
 * their wake windows. Only now does the coordinator change its own channel. */
static void channel_migration_commit_if_ready(void)
{
    unsigned char target = 0U;
    unsigned char prepared = 0U;
    unsigned char total = 0U;
    int should_commit = 0;
    unsigned long now = (unsigned long)mg_millis();

    pthread_mutex_lock(&g_uart_mutex);
    int uart_available = g_uart_fd >= 0;
    pthread_mutex_unlock(&g_uart_mutex);
    if (!uart_available) return;

    pthread_mutex_lock(&g_channel_operation_mutex);
    if (g_channel_operation.phase == CHANNEL_OPERATION_PENDING &&
        g_channel_operation.waiting_for_nodes &&
        !g_channel_operation.commit_sent &&
        g_channel_operation.prepared_count == g_channel_operation.node_count)
    {
        g_channel_operation.waiting_for_nodes = 0U;
        g_channel_operation.commit_sent = 1U;
        g_channel_operation.coordinator_status = 0xFFU;
        g_channel_operation.next_query_ms = now + 500U;
        g_channel_operation.deadline_ms = now + 20000U;
        snprintf(g_channel_operation.message, sizeof(g_channel_operation.message),
                 "%s", "全部节点已准备，协调器正在执行最终切换");
        target = g_channel_operation.target_channel;
        prepared = g_channel_operation.prepared_count;
        total = g_channel_operation.node_count;
        should_commit = 1;
    }
    pthread_mutex_unlock(&g_channel_operation_mutex);
    if (!should_commit) return;
    persist_channel_operation_snapshot();

    unsigned char payload[4] = {1U, target, 0U, GW_CHANNEL_PHASE_COMMIT};
    pthread_mutex_lock(&g_uart_mutex);
    int available = g_uart_fd >= 0;
    if (available)
        send_hex_frame_to_uart(g_uart_fd, GW_CMD_CHANNEL_CONFIG,
                               sizeof(payload), payload);
    pthread_mutex_unlock(&g_uart_mutex);

    if (!available)
    {
        pthread_mutex_lock(&g_channel_operation_mutex);
        g_channel_operation.waiting_for_nodes = 1U;
        g_channel_operation.commit_sent = 0U;
        g_channel_operation.next_query_ms = 0U;
        g_channel_operation.deadline_ms = 0U;
        snprintf(g_channel_operation.message,
                 sizeof(g_channel_operation.message), "%s",
                 "全部节点已准备，等待协调器串口恢复");
        pthread_mutex_unlock(&g_channel_operation_mutex);
        persist_channel_operation_snapshot();
        return;
    }
    publish_channel_operation_event(CHANNEL_OPERATION_PENDING, target, 0U,
                                    0xFFU, prepared, total,
                                    "全部节点已准备，协调器正在执行最终切换");
    printf("[Zigbee信道迁移] %u/%u 个节点已准备，已通知协调器最终切换至信道%u。\n",
           prepared, total, target);
}

/* 0x99 is sent while the terminal keeps its receiver on. Use that window to
 * deliver only this terminal's standard Mgmt_NWK_Update; the coordinator
 * remains on the old channel until every listed terminal is prepared. */
static void channel_migration_note_node_awake(uint16_t short_addr)
{
    char short_text[16];
    char mac[32] = {0};
    snprintf(short_text, sizeof(short_text), "%04X", short_addr);
    if (!get_mac_by_short_addr(short_text, mac)) return;

    unsigned char target = 0U;
    int should_send = 0;
    pthread_mutex_lock(&g_channel_operation_mutex);
    if (g_channel_operation.phase == CHANNEL_OPERATION_PENDING &&
        g_channel_operation.waiting_for_nodes)
    {
        for (unsigned char i = 0U; i < g_channel_operation.node_count; ++i)
        {
            ChannelMigrationNode_t *node = &g_channel_operation.nodes[i];
            if (strcasecmp(node->mac, mac) == 0 &&
                node->state == CHANNEL_NODE_WAITING)
            {
                node->short_addr = short_addr;
                node->state = CHANNEL_NODE_SENT;
                target = g_channel_operation.target_channel;
                should_send = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_channel_operation_mutex);
    if (!should_send) return;
    persist_channel_operation_snapshot();

    unsigned char payload[6] = {
        1U, target, 1U, GW_CHANNEL_PHASE_PREPARE,
        (unsigned char)(short_addr & 0xFFU),
        (unsigned char)(short_addr >> 8U)
    };
    pthread_mutex_lock(&g_uart_mutex);
    int available = g_uart_fd >= 0;
    if (available)
        send_hex_frame_to_uart(g_uart_fd, GW_CMD_CHANNEL_CONFIG,
                               sizeof(payload), payload);
    pthread_mutex_unlock(&g_uart_mutex);

    if (!available)
    {
        pthread_mutex_lock(&g_channel_operation_mutex);
        for (unsigned char i = 0U; i < g_channel_operation.node_count; ++i)
            if (strcasecmp(g_channel_operation.nodes[i].mac, mac) == 0 &&
                g_channel_operation.nodes[i].state == CHANNEL_NODE_SENT)
                g_channel_operation.nodes[i].state = CHANNEL_NODE_WAITING;
        pthread_mutex_unlock(&g_channel_operation_mutex);
        persist_channel_operation_snapshot();
        return;
    }
    printf("[Zigbee信道迁移] 节点%s(0x%04X)已唤醒，正在下发信道%u迁移通知。\n",
           mac, short_addr, target);
}

static void channel_migration_remove_node(const char *mac)
{
    int changed = 0;
    unsigned char target = 0U;
    unsigned char current = 0U;
    unsigned char prepared = 0U;
    unsigned char total = 0U;
    pthread_mutex_lock(&g_channel_operation_mutex);
    if (g_channel_operation.phase == CHANNEL_OPERATION_PENDING &&
        g_channel_operation.waiting_for_nodes)
    {
        for (unsigned char i = 0U; i < g_channel_operation.node_count; ++i)
        {
            if (strcasecmp(g_channel_operation.nodes[i].mac, mac) != 0) continue;
            if (g_channel_operation.nodes[i].state == CHANNEL_NODE_PREPARED &&
                g_channel_operation.prepared_count > 0U)
                g_channel_operation.prepared_count--;
            for (unsigned char j = i + 1U; j < g_channel_operation.node_count; ++j)
                g_channel_operation.nodes[j - 1U] = g_channel_operation.nodes[j];
            g_channel_operation.node_count--;
            snprintf(g_channel_operation.message,
                     sizeof(g_channel_operation.message),
                     "已通知%u/%u个节点，等待其余节点唤醒",
                     g_channel_operation.prepared_count,
                     g_channel_operation.node_count);
            target = g_channel_operation.target_channel;
            current = g_channel_operation.current_channel;
            prepared = g_channel_operation.prepared_count;
            total = g_channel_operation.node_count;
            changed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_channel_operation_mutex);
    if (changed)
    {
        persist_channel_operation_snapshot();
        char progress[96];
        snprintf(progress, sizeof(progress),
                 "已通知%u/%u个节点，等待其余节点唤醒",
                 prepared, total);
        publish_channel_operation_event(CHANNEL_OPERATION_PENDING, target,
                                        current, 0xFFU, prepared, total,
                                        progress);
        channel_migration_commit_if_ready();
    }
}

static void persist_channel_operation_snapshot(void)
{
    ChannelOperationRecord_t record;
    ChannelOperationNodeRecord_t nodes[CHANNEL_MIGRATION_MAX_NODES];
    memset(&record, 0, sizeof(record));

    pthread_mutex_lock(&g_channel_operation_mutex);
    snprintf(record.state, sizeof(record.state), "%s",
             channel_operation_phase_text(g_channel_operation.phase));
    record.strategy = g_channel_operation.strategy;
    record.target_channel = g_channel_operation.target_channel;
    record.current_channel = g_channel_operation.current_channel;
    record.coordinator_status = g_channel_operation.coordinator_status;
    record.waiting_for_nodes = g_channel_operation.waiting_for_nodes;
    record.commit_sent = g_channel_operation.commit_sent;
    record.node_count = g_channel_operation.node_count;
    record.prepared_count = g_channel_operation.prepared_count;
    snprintf(record.message, sizeof(record.message), "%s",
             g_channel_operation.message);
    for (unsigned char i = 0U; i < g_channel_operation.node_count; ++i)
    {
        snprintf(nodes[i].mac, sizeof(nodes[i].mac), "%s",
                 g_channel_operation.nodes[i].mac);
        nodes[i].short_addr = g_channel_operation.nodes[i].short_addr;
        nodes[i].state = g_channel_operation.nodes[i].state;
    }
    pthread_mutex_unlock(&g_channel_operation_mutex);

    char database_error[160] = {0};
    if (save_channel_operation_snapshot(&record, nodes, record.node_count,
                                        database_error,
                                        sizeof(database_error)) != 0)
    {
        printf("⚠️ [Zigbee信道迁移] 进度快照持久化失败：%s；"
               "本次内存中的迁移流程继续执行。\n",
               database_error[0] ? database_error : "SQLite 未返回详细原因");
    }
}

static void restore_channel_operation(void)
{
    ChannelOperationRecord_t record;
    ChannelOperationNodeRecord_t *nodes = NULL;
    int node_count = 0;
    if (load_channel_operation_record(&record) != 0 ||
        load_channel_operation_nodes(&nodes, &node_count) != 0)
    {
        free(nodes);
        return;
    }

    ChannelOperationPhase_t phase = CHANNEL_OPERATION_IDLE;
    if (strcmp(record.state, "pending") == 0) phase = CHANNEL_OPERATION_PENDING;
    else if (strcmp(record.state, "success") == 0) phase = CHANNEL_OPERATION_SUCCESS;
    else if (strcmp(record.state, "failed") == 0) phase = CHANNEL_OPERATION_FAILED;

    int normalized_sent_state = 0;
    pthread_mutex_lock(&g_channel_operation_mutex);
    memset(&g_channel_operation, 0, sizeof(g_channel_operation));
    g_channel_operation.phase = phase;
    g_channel_operation.strategy = (unsigned char)record.strategy;
    g_channel_operation.target_channel = (unsigned char)record.target_channel;
    g_channel_operation.current_channel = (unsigned char)record.current_channel;
    g_channel_operation.coordinator_status = (unsigned char)record.coordinator_status;
    g_channel_operation.waiting_for_nodes = (unsigned char)record.waiting_for_nodes;
    g_channel_operation.commit_sent = (unsigned char)record.commit_sent;
    snprintf(g_channel_operation.message, sizeof(g_channel_operation.message),
             "%s", record.message);
    if (node_count > (int)CHANNEL_MIGRATION_MAX_NODES)
        node_count = (int)CHANNEL_MIGRATION_MAX_NODES;
    g_channel_operation.node_count = (unsigned char)node_count;
    g_channel_operation.prepared_count = 0U;
    for (int i = 0; i < node_count; ++i)
    {
        snprintf(g_channel_operation.nodes[i].mac,
                 sizeof(g_channel_operation.nodes[i].mac), "%s", nodes[i].mac);
        g_channel_operation.nodes[i].short_addr = nodes[i].short_addr;
        /* A SENT command may already have moved the terminal before the Linux
         * process stopped. Treat it as prepared on resume to avoid deadlock. */
        g_channel_operation.nodes[i].state = nodes[i].state == CHANNEL_NODE_SENT
            ? CHANNEL_NODE_PREPARED : nodes[i].state;
        if (nodes[i].state == CHANNEL_NODE_SENT) normalized_sent_state = 1;
        if (g_channel_operation.nodes[i].state == CHANNEL_NODE_PREPARED)
            g_channel_operation.prepared_count++;
    }
    if (phase == CHANNEL_OPERATION_PENDING && record.commit_sent)
    {
        /* COMMIT is idempotent: resend it after a process restart. This closes
         * the crash window between persisting commit_sent and writing UART. */
        g_channel_operation.waiting_for_nodes = 1U;
        g_channel_operation.commit_sent = 0U;
        g_channel_operation.next_query_ms = 0U;
        g_channel_operation.deadline_ms = 0U;
        normalized_sent_state = 1;
    }
    pthread_mutex_unlock(&g_channel_operation_mutex);
    free(nodes);

    if (normalized_sent_state) persist_channel_operation_snapshot();
    if (phase == CHANNEL_OPERATION_PENDING)
    {
        printf("♻️ [Zigbee信道迁移] 已恢复等待任务：目标信道=%u，节点进度=%u/%u。\n",
               g_channel_operation.target_channel,
               g_channel_operation.prepared_count,
               g_channel_operation.node_count);
        channel_migration_commit_if_ready();
    }
}

static void publish_monitor_event(const char *mac, int enabled,
                                  const char *state, uint32_t batch_id,
                                  const unsigned char *telemetry)
{
    char extra[160] = "";
    if (telemetry != NULL)
        snprintf(extra, sizeof(extra),
                 ",\"power_source\":%u,\"lqi\":%u,\"rssi\":%d,\"link_updated_at\":%lld",
                 telemetry[0], telemetry[1], (int)(int8_t)telemetry[2],
                 (long long)time(NULL));
    pthread_mutex_lock(&g_ws_mutex);
    snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer),
             "{\"type\":\"monitor_update\",\"mac\":\"%s\","
             "\"monitoring_enabled\":%s,\"collection_state\":\"%s\","
             "\"batch_id\":%u,\"captured_at\":%lld%s}",
             mac, enabled ? "true" : "false", state, batch_id,
             get_collection_capture_time(mac, batch_id), extra);
    queue_ws_event_locked();
    pthread_mutex_unlock(&g_ws_mutex);
}

/* 数据库已经提交本批传感器结果后再通知前端，避免“采集完成”状态先于
 * 0x8C 振动特征摘要到达时，前端误读到上一批数据。 */
static void publish_collection_data_ready_event(const char *mac,
                                                uint32_t batch_id,
                                                int raw_complete)
{
    gateway_log_event("INFO", "backend", raw_complete ? "waveform_saved" : "vibration_data_saved", mac, -1, batch_id,
                      "%s", raw_complete ? "原始振动波形已接收并保存" : "振动批次或特征数据已接收并保存（不代表整批上传完成）");
    pthread_mutex_lock(&g_ws_mutex);
    snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer),
             "{\"type\":\"collection_data_ready\",\"mac\":\"%s\","
             "\"batch_id\":%u,\"raw_complete\":%s}",
             mac, batch_id, raw_complete ? "true" : "false");
    queue_ws_event_locked();
    pthread_mutex_unlock(&g_ws_mutex);
}

static void publish_vibration_upload_progress(const char *mac,
                                              uint32_t batch_id,
                                              uint16_t received,
                                              uint16_t total)
{
    pthread_mutex_lock(&g_ws_mutex);
    snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer),
             "{\"type\":\"vibration_upload_progress\",\"mac\":\"%s\","
             "\"batch_id\":%u,\"received_packets\":%u,\"total_packets\":%u,"
             "\"status\":\"uploading\"}", mac, batch_id, received, total);
    queue_ws_event_locked();
    pthread_mutex_unlock(&g_ws_mutex);
}

static int lookup_mac_by_short(uint16_t short_addr, char mac[24])
{
    char short_text[16];
    snprintf(short_text, sizeof(short_text), "%04X", short_addr);
    return get_mac_by_short_addr(short_text, mac);
}

static void trace_config_uart(unsigned char cmd, const unsigned char *payload, unsigned char len)
{
    if (!g_config_diag_enabled || (cmd != GW_RSP_CONFIG_QUERY &&
        cmd != GW_RSP_COMMAND_QUEUED && cmd != GW_RSP_AF_CONFIRM)) return;
    char mac[24] = {0};
    if (payload && len >= 2U) (void)lookup_mac_by_short(read_le16(payload), mac);
    config_diag_frame(mg_millis(), cmd, payload, len, mac);
}

static int send_monitor_control(uint16_t short_addr, unsigned char operation,
                                unsigned char enabled)
{
    unsigned char payload[4];
    write_le16(payload, short_addr);
    payload[2] = operation;
    payload[3] = enabled;
    pthread_mutex_lock(&g_uart_mutex);
    int available = g_uart_fd >= 0;
    if (available)
    {
        send_hex_frame_to_uart(g_uart_fd, GW_CMD_MONITOR_CONTROL,
                               operation == MONITOR_OP_SET_ENABLED ? 4 : 3,
                               payload);
    }
    else printf("[监测命令] 发送失败: 协调器串口未连接\n");
    pthread_mutex_unlock(&g_uart_mutex);
    return available;
}

static int build_monitor_config_payload(uint16_t short_addr, const MonitorConfig_t *cfg,
                                       unsigned char payload[31], char mac[32])
{
    unsigned char transaction_flags = 0U;
    unsigned char migration_channel = 0U;
    mac[0] = 0;
    /* Discovery and link telemetry may arrive before the user grants admission. */
    if (!lookup_mac_by_short(short_addr, mac) || !device_is_authorized(mac)) return 0;

    int enabled = 0;
    if (get_device_monitoring(mac, &enabled))
        transaction_flags |= 0x06U; /* authorized, monitoring state present */
    pthread_mutex_lock(&g_channel_operation_mutex);
    if (g_channel_operation.phase == CHANNEL_OPERATION_PENDING)
    {
        for (unsigned char i = 0U; i < g_channel_operation.node_count; ++i)
        {
            if (strcasecmp(g_channel_operation.nodes[i].mac, mac) == 0)
            {
                transaction_flags |= 0x01U;
                migration_channel = g_channel_operation.target_channel;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_channel_operation_mutex);
    write_le16(payload, short_addr);
    write_le32(payload + 2, cfg->auto_interval_sec);
    write_le16(payload + 6, cfg->poll_interval_sec);
    write_le16(payload + 8, cfg->vibration_rate_hz);
    write_le16(payload + 10, cfg->digital_mic_rate_hz);
    write_le32(payload + 12, cfg->analog_mic_rate_hz);
    write_le16(payload + 16, cfg->vibration_window_ms);
    write_le16(payload + 18, cfg->digital_mic_window_ms);
    write_le16(payload + 20, cfg->analog_mic_window_ms);
    write_le16(payload + 22, cfg->raw_upload_every_cycles);
    write_le32(payload + 24, cfg->raw_upload_generation);
    /* Bytes 28..29 used to carry a user-selected receive window. They now
     * describe the transaction following this reply. Bit 0 keeps the node
     * awake until a later query succeeds after channel migration. */
    payload[28] = transaction_flags;
    payload[29] = migration_channel;
    payload[30] = cfg->sensor_enable_mask;
    return 1;
}

/* HTTP and UART threads share push throttling. Never keep this lock while
 * touching SQLite or UART. Latest intent always wins over an older hash. */
static pthread_mutex_t control_delivery_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { char mac[32]; uint32_t hash; uint64_t sent_at; int confirmed; }
    control_deliveries[128];
static int control_delivery(const char *mac, uint32_t hash, int operation)
{
    uint64_t now = mg_millis();
    unsigned slot = 0;
    pthread_mutex_lock(&control_delivery_mutex);
    for (unsigned i=0; i<128; i++) {
        if (!strcasecmp(control_deliveries[i].mac,mac)) { slot=i; break; }
        if (!control_deliveries[i].mac[0] ||
            control_deliveries[i].sent_at < control_deliveries[slot].sent_at) slot=i;
    }
    int same = !strcasecmp(control_deliveries[slot].mac,mac) &&
               control_deliveries[slot].hash == hash;
    int result = 1;
    if (operation == 0 && same && now-control_deliveries[slot].sent_at < 5000U)
        result = 0;
    else if (operation == 2) {
        result = !same || !control_deliveries[slot].confirmed;
        snprintf(control_deliveries[slot].mac,sizeof(control_deliveries[slot].mac),"%s",mac);
        control_deliveries[slot].hash=hash;
        control_deliveries[slot].confirmed=1;
    } else {
        snprintf(control_deliveries[slot].mac,sizeof(control_deliveries[slot].mac),"%s",mac);
        control_deliveries[slot].hash=hash;
        control_deliveries[slot].sent_at=now;
        control_deliveries[slot].confirmed=0;
    }
    pthread_mutex_unlock(&control_delivery_mutex);
    return result;
}

/* Only a post-upload status report can open this bounded delivery window. */
static int send_monitor_config_scoped(uint16_t address,const MonitorConfig_t *cfg)
{
    unsigned char payload[32];char mac[32];
    if (!build_monitor_config_payload(address,cfg,payload,mac)) return 0;
    payload[31]=CONTROL_DELIVERY_WINDOW;
    pthread_mutex_lock(&g_uart_mutex);
    int available=g_uart_fd>=0;
    config_diag_tx(mg_millis(),payload,31,mac,available);
    if(available) {
        (void)control_delivery(mac,control_fingerprint(payload+2,29),1);
        send_hex_frame_to_uart(g_uart_fd,CONTROL_CONFIG_SYNC_CMD,sizeof(payload),payload);
    }
    pthread_mutex_unlock(&g_uart_mutex);
    if(available && mac[0] && g_mqtt_store_ready &&
       mqtt_store_policy(g_db,mac,cfg->sensor_enable_mask)!=0)
        gateway_log_event("ERROR","backend","mqtt_policy_snapshot_failed",mac,address,-1,
                          "MQTT传感器策略快照保存失败，相关批次将保守判定");
    return available;
}


#define GATEWAY_MQTT_BATCH_EVENT(mac,batch,raw) do { \
    mqtt_note_batch(mac,batch,(raw)?MQTT_BATCH_RAW:MQTT_BATCH_VIBRATION); \
} while(0)
static void log_scalar_capture_missing(const char *mac,uint16_t address,uint32_t batch,uint8_t unavailable)
{
    static const char *names[]={"温度","磁场XYZ","数字麦克风声级","超声麦克风声级"};
    char fields[160]={0};size_t used=0;
    for(unsigned i=0;i<4;i++)if(unavailable&(1U<<i))
        used+=(size_t)snprintf(fields+used,sizeof(fields)-used,"%s%s",used?"、":"",names[i]);
    printf("⚠ [节点采集缺项] MAC=%s | 批次=%u | 已启用但未采到：%s | 跳过这些项，继续校验并上传本批其余数据\n",
           mac,batch,fields);
    gateway_log_event("WARN","node","sensor_capture_missing",mac,address,batch,
                      "本批采集时已启用但未采到：%s；跳过这些项，继续校验并上传本批其余数据",fields);
}
#define GATEWAY_SCALAR_UNAVAILABLE_EVENT(mac,address,batch,unavailable) \
    log_scalar_capture_missing(mac,address,batch,unavailable)
#define GATEWAY_SCALAR_POLICY_EVENT(mac,batch,required,unavailable) \
    (!g_mqtt_store_ready ? -1 : mqtt_store_batch_scalar_status(g_db,mac,batch,required,unavailable))
#define UPLOAD_RESTART_GUARD_MS 35000U
static uint32_t upload_timing_unit(const char *mac,unsigned raw)
{
    UploadTimingRecord_t timing;
    int found=get_upload_timing(mac,raw,&timing);
    if(found<0) {
        static uint64_t last_error;
        uint64_t now=mg_millis();
        if(!last_error || now-last_error>=30000U) {
            last_error=now;
            fprintf(stderr,"[上传估时] 读取历史耗时失败，使用默认估计 | MAC=%s\n",mac);
            gateway_log_event("WARN","backend","upload_timing_read_failed",mac,-1,-1,
                              "读取节点历史上传耗时失败，使用默认估计");
        }
    }
    return found==1?timing.mean_unit_ms:0U;
}
struct UploadAdmissionEntry;
static void upload_timing_sample(const struct UploadAdmissionEntry *entry,uint64_t elapsed);
#include "upload_trace_backend.h"
#define UPLOAD_ADMISSION_UNIT_MS(mac,raw) upload_timing_unit(mac,raw)
#define UPLOAD_ADMISSION_SAMPLE(entry,elapsed) upload_timing_sample(entry,elapsed)
#define UPLOAD_ADMISSION_EVENT(entry,reason,now) do { \
    if((entry)->active && !strcmp(reason,"60秒无上传活动，许可过期"))upload_trace_expired((entry)->mac,(entry)->batch); \
    printf("[上传名额] MAC=%s | 批次=%u | %s | 距最后上行=%llu ms\n", \
      (entry)->mac,(entry)->batch,reason,(unsigned long long)((now)-(entry)->touched)); \
    gateway_log_event("INFO","backend","upload_reservation",(entry)->mac,-1,(entry)->batch, \
      "%s；距最后上传活动=%llu ms",reason,(unsigned long long)((now)-(entry)->touched)); \
} while(0)
#include "vibration_gateway.h"

static void upload_timing_sample(const UploadAdmissionEntry *entry,uint64_t elapsed)
{
    const char *mac=entry->mac;
    uint32_t batch=entry->batch;
    unsigned raw=entry->raw_required;
    UploadTimingRecord_t timing;
    int saved=save_upload_timing(mac,batch,raw,entry->blocks,elapsed,&timing);
    if(saved<0) {
        fprintf(stderr,"[上传估时] 保存历史耗时失败 | MAC=%s | 批次=%u\n",mac,batch);
        gateway_log_event("WARN","backend","upload_timing_save_failed",mac,-1,batch,
                          "保存节点历史耗时失败，本批采集数据及完成确认不受影响");
    } else if(saved) {
        char scalar_timing[128]="";
        if(entry->scalar_verified_ms) {
            unsigned long long scalar_elapsed=entry->scalar_verified_ms-entry->started;
            snprintf(scalar_timing,sizeof(scalar_timing),
                     " | 标量上传（温度、数字麦克风、超声麦克风、磁场）=%llu ms",scalar_elapsed);
        }
        printf("[上传耗时统计] MAC=%s | 批次=%u | 模式=%s | 本次=%llu ms | 平均%s=%u ms | 成功样本=%u | 已保存%s\n",
               mac,batch,raw?"原始波形":"仅特征",(unsigned long long)elapsed,
               raw?"每窗口":"每批",timing.mean_unit_ms,timing.samples,scalar_timing);
        gateway_log_event("INFO","backend","upload_timing_saved",mac,-1,batch,
                          "%s上传完成，本次=%llu ms，平均%s=%u ms，成功样本=%u；估时已持久化%s",
                          raw?"原始波形":"仅特征",(unsigned long long)elapsed,
                          raw?"每窗口":"每批",timing.mean_unit_ms,timing.samples,scalar_timing);
    }
}

//自定义名称校验：去除首尾空格、限制 32 个字符、拒绝控制字符。
static int normalize_custom_name(const char *input, char *output, size_t output_size)
{
    if (!input || !output || output_size == 0)
    {
        return -1;
    }

    while (*input && isspace((unsigned char)*input))
    {
        input++;
    }

    const char *end = input + strlen(input);
    while (end > input && isspace((unsigned char)end[-1]))
    {
        end--;
    }

    size_t byte_length = (size_t)(end - input);
    if (byte_length >= output_size)
    {
        return -1;
    }

    size_t character_count = 0;
    for (const unsigned char *p = (const unsigned char *)input;
         p < (const unsigned char *)end;
         p++)
    {
        if (*p < 0x20 || *p == 0x7F)
        {
            return -1;
        }

        if ((*p & 0xC0) != 0x80)
        {
            character_count++;
        }
    }

    if (character_count > 32)
    {
        return -1;
    }

    memcpy(output, input, byte_length);
    output[byte_length] = '\0';
    return 0;
}

//名称更新 WebSocket 通知，同时发送显示名称、产品型号和自定义名称。
static void publish_name_update(const char *mac,const char *model_name,const char *custom_name,const char *display_name)
{
    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "type", "name_update");
    cJSON_AddStringToObject(event, "mac", mac);
    cJSON_AddStringToObject(event, "name", display_name);
    cJSON_AddStringToObject(event, "model_name", model_name);
    cJSON_AddStringToObject(event, "custom_name", custom_name);

    char *json = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);

    pthread_mutex_lock(&g_ws_mutex);
    snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer), "%s", json ? json : "{}");
    queue_ws_event_locked();
    pthread_mutex_unlock(&g_ws_mutex);

    if (json) free(json);
}

// 串口路径只从配置文件加载；不使用硬编码默认值或自动替换配置。
static int load_gateway_config(void)
{
    g_serial_path[0] = '\0';
    FILE *file = fopen(GATEWAY_CONFIG_FILE, "rb");
    if (!file)
    {
        printf("⚠️ [串口配置] 无法读取 %s，请设置 serial_path 或在系统设置页配置串口。\n", GATEWAY_CONFIG_FILE);
        return -1;
    }

    char buffer[65537] = {0};
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    int read_failed=ferror(file) || fgetc(file)!=EOF;
    fclose(file);
    buffer[length] = '\0';

    cJSON *root = read_failed ? NULL : cJSON_ParseWithLengthOpts(buffer,length+1,NULL,1);
    cJSON *serial_path = root ? cJSON_GetObjectItem(root, "serial_path") : NULL;
    int valid = cJSON_IsString(serial_path) &&
        strncmp(serial_path->valuestring, "/dev/", 5) == 0 &&
        strlen(serial_path->valuestring) > 5 &&
        strlen(serial_path->valuestring) < sizeof(g_serial_path);
    if (valid)
    {
        snprintf(g_serial_path, sizeof(g_serial_path), "%s", serial_path->valuestring);
    }
    if (root) cJSON_Delete(root);
    if (!valid)
        printf("⚠️ [串口配置] %s 的 serial_path 无效，请填写完整的 /dev/ 设备路径。\n", GATEWAY_CONFIG_FILE);
    return valid ? 0 : -1;
}

// 串口路径是网关本机配置，保存到运行目录下的JSON文件
static int save_gateway_config(void)
{
    /* Preserve MQTT credentials/options and any unrelated configuration when
     * the existing serial settings page saves a new device path. */
    cJSON *root=NULL;
    FILE *previous=fopen(GATEWAY_CONFIG_FILE,"rb");
    if(previous) {
        char *buffer=malloc(65537);
        if(!buffer){fclose(previous);return -1;}
        size_t n=fread(buffer,1,65536,previous);int bad=ferror(previous) || fgetc(previous)!=EOF;
        fclose(previous);buffer[n]=0;
        if(!bad)root=cJSON_ParseWithLengthOpts(buffer,n+1,NULL,1);
        free(buffer);
        if(!cJSON_IsObject(root)){cJSON_Delete(root);return -1;}
    } else if(errno==ENOENT)root=cJSON_CreateObject();
    if(!root)return -1;
    cJSON_DeleteItemFromObjectCaseSensitive(root,"serial_path");
    cJSON_DeleteItemFromObjectCaseSensitive(root,"baud_rate");
    cJSON_AddStringToObject(root,"serial_path",g_serial_path);
    cJSON_AddNumberToObject(root,"baud_rate",GATEWAY_BAUD_RATE);
    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return -1;

    const char *temporary=GATEWAY_CONFIG_FILE ".tmp";
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC,0600);
    FILE *file=fd>=0?fdopen(fd,"wb"):NULL;
    if (!file)
    {
        if(fd>=0)close(fd);
        free(json);
        return -1;
    }

    size_t length = strlen(json);
    int result = fwrite(json, 1, length, file) == length ? 0 : -1;
    if(fflush(file)!=0 || fsync(fd)!=0)result=-1;
    if(fclose(file)!=0)result=-1;
    if(!result && rename(temporary,GATEWAY_CONFIG_FILE)!=0)result=-1;
    if(result)unlink(temporary);
    free(json);
    return result;
}

// 向协调器查询当前PAN ID和实际工作信道
static void request_network_info(void)
{
    pthread_mutex_lock(&g_uart_mutex);
    if (g_uart_fd >= 0) {
        const unsigned char enable = V2_WINDOW_QUERY_TAG_ENABLE;
        send_hex_frame_to_uart(g_uart_fd, 0x0A, 0, NULL);
        send_hex_frame_to_uart(g_uart_fd, RADIO_DIAG_CMD, 1, &enable);
    }
    pthread_mutex_unlock(&g_uart_mutex);
}

static void refresh_device_activity_by_short(uint16_t short_addr)
{
    char mac[24] = {0};
    if (!lookup_mac_by_short(short_addr, mac)) return;
    char short_text[16];
    snprintf(short_text, sizeof(short_text), "%04X", short_addr);
    (void)update_device_online(mac, short_text);
}

static int retry_removed_device_by_short(uint16_t short_addr,
                                         unsigned char trigger_cmd)
{
    RevokedNodeRecord_t removed;
    if (!get_revoked_node_by_short(short_addr, &removed)) return 0;

    long long now = (long long)time(NULL);
    if (removed.last_leave_at == 0 ||
        now < removed.last_leave_at ||
        now - removed.last_leave_at >= REMOVE_RETRY_INTERVAL_SEC)
    {
        unsigned char mac_bin[8];
        if (mac_string_to_bytes(removed.mac, mac_bin) == 0)
        {
            int submitted = submit_removed_device_leave(mac_bin);
            if (submitted) (void)mark_revoked_leave_attempt(removed.mac, short_addr);
            gateway_log_event(submitted ? "INFO" : "WARN", "backend", "removal_retry", removed.mac, short_addr, -1,
                              "检测到撤销节点上行，离网指令%s；触发命令=0x%02X",
                              submitted ? "已提交协调器，等待执行结果" : "串口提交失败", trigger_cmd);
            printf("[设备移除重试] MAC=%s | 短地址=0x%04X | 触发报文=0x%02X | %s\n",
                   removed.mac, short_addr, trigger_cmd,
                   submitted ? "离网请求已提交串口" : "串口提交失败，保留离网任务");
        }
    }
    /* 即使本次受3秒节流限制，也要阻止撤销节点进入业务处理函数。 */
    return 1;
}

/*********************************************************************
 * 主函数 (Main)
 *********************************************************************/
static volatile sig_atomic_t gateway_stopping;
static void gateway_stop_signal(int signal_number) { (void)signal_number; gateway_stopping = 1; }

int main(void)
{
    console_log_start();
    /* Single-node block uploader needs no epoch, time slots or quarantine. */
    mg_log_set(MG_LL_NONE); // 用于禁止打印 http 日志

    printf("=========================================\n");
    printf("[系统] 🚀 工业级高性能边缘网关网络内核启动\n");
    printf("=========================================\n");
    config_diag_start(getenv("ZIGBEE_CONFIG_DIAG"), mg_millis());
    const char *radio_diag_env=getenv("ZIGBEE_RADIO_DIAG");
    g_radio_diag_enabled=radio_diag_env && strcmp(radio_diag_env,"1")==0;
    if(g_radio_diag_enabled)printf("[无线诊断] 已请求开启被动MAC/AF观察；等待协调器能力回报，不能用未观测到轮询推断节点没有取包\n");
    config_diag_print(mg_millis(), "已开启 | RX99 / TX0C / SUBMIT8E / CONFIRM90 | 仅记录终端配置收发，不打印密钥 | 原有“事务号”字段在TI桥接中为ZCL序号，不能精确配对并发请求");

    // 💡 新增：在系统启动的最开始，初始化数据库
    if (init_database() != 0) {
        printf("❌ [系统错误] 数据库启动失败，网关退出！\n");
        return -1;
    }
    int logs_ready = gateway_log_init("data/gateway-logs.db") == 0;
    gateway_log_event("INFO", "backend", "system_start", NULL, -1, -1, "网关后端启动");
    signal(SIGINT, gateway_stop_signal);
    signal(SIGTERM, gateway_stop_signal);
    if (reset_zigbee_channel_config() == 0)
        printf("[Zigbee信道策略] 后端启动，数据库策略已重置为自动选择。\n");
    else
        printf("⚠️ [Zigbee信道策略] 数据库默认策略重置失败。\n");

    // 1. 加载本机配置并初始化串口设备
    int serial_config_ready = load_gateway_config() == 0;
    g_uart_fd = serial_config_ready ? uart_init(g_serial_path, GATEWAY_BAUD_RATE) : -1;
    request_radio_diagnostics();
    gateway_log_event(g_uart_fd < 0 ? "ERROR" : "INFO", "backend", "serial_open", NULL, -1, -1,
                      "协调器串口%s：%s", g_uart_fd < 0 ? "打开失败" : "已打开", g_serial_path);

    // 串口失败时仍启动HTTP服务，用户可以在系统设置页修正路径
    if (g_uart_fd < 0) 
    {
        printf("⚠️ [系统警告] 串口 %s 初始化失败，请在系统设置中修正。\n", g_serial_path);
    }

    /* A migration may span several node collection periods. Resume it only
     * after the coordinator UART is available so already-prepared terminals
     * are never abandoned on the target channel. */
    restore_channel_operation();

    /* MQTT owns network objects on this thread only; UART merely records
     * durable batch facts. Initialize those tables before starting UART. */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    GatewayMqttConfig mqtt_config;
    if(gateway_mqtt_load_config(GATEWAY_CONFIG_FILE,&mqtt_config)!=0)
        fprintf(stderr,"[MQTT] 配置无效或无法读取，MQTT已停用；串口/HTTP继续运行\n");
    GatewayMqtt mqtt;
    (void)gateway_mqtt_init(&mqtt,&mgr,g_db,&mqtt_config);
    gateway_mqtt_api_init(&g_mqtt_api,&mqtt,GATEWAY_CONFIG_FILE);
    g_mqtt_store_ready=mqtt.store_ready;

    // 创建监听线程！
    pthread_t tid;
    if (pthread_create(&tid, NULL, uart_rx_thread_func, &g_uart_fd) != 0) 
    {
        perror("[系统错误] 无法创建串口监听线程");
        return -1;
    } 
    else 
    {
        pthread_detach(tid);
    }

    request_network_info();

    //初始化网络事件服务器

    //相当于注册了一个请求处理函数
    mg_http_listen(&mgr, "http://0.0.0.0:8080", ev_handler, NULL);

    printf("[系统] 🟢 网关已完全就绪，开始进入高性能网络监听状态...\n");

    // Mongoose 得以独占 CPU 时间片，网络响应达到极速，且在无请求时自动进入系统休眠
    uint64_t last_log_flush = 0, last_log_error = 0;
    while (!gateway_stopping)
    {
        mg_mgr_poll(&mgr, 100); //当100ms没有接收到请求就陷入沉睡，有请求时激活ev_handler函数来处理请求
        uint64_t now_ms = mg_millis();
        gateway_mqtt_poll(&mqtt,now_ms);
        gateway_mqtt_api_poll(&g_mqtt_api,now_ms);
        if (logs_ready && now_ms - last_log_flush >= 500) {
            if (gateway_log_flush() < 0 && (last_log_error == 0 || now_ms - last_log_error >= 10000)) {
                fprintf(stderr, "[日志] 保存失败，将保留队列等待重试\n");
                last_log_error = now_ms;
            }
            last_log_flush = now_ms;
        }
    }

    gateway_log_event("INFO", "backend", "system_stop", NULL, -1, -1, "网关后端正常停止");
    gateway_mqtt_api_stop(&g_mqtt_api);
    gateway_mqtt_stop(&mqtt);
    gateway_log_close();
    mg_mgr_free(&mgr);
    return 0;
}

// 初始化解析器
void uart_parser_init(UartParser_t *parser) 
{
    parser->state = STATE_SOF;
    parser->rx_cmd = 0;
    parser->payload_len = 0;
    parser->payload_idx = 0;
    parser->bcc_calc = 0;
}

/*********************************************************************
 * 后台串口监听线程 (语义化状态机)
 * 格式: [0xFE] [CMD] [LEN] [Payload] [BCC]
 *********************************************************************/
void *uart_rx_thread_func(void *arg) 
{
    (void)arg; // 告诉编译器：我知道传进来了参数，我故意不用的，别警告我
    unsigned char buf[128];
    
    // 实例化一个状态机大脑，并初始化
    UartParser_t parser;
    uart_parser_init(&parser);

    printf("[系统] 🧵 后台串口监听已启动，等待数据流...\n");

    while (1) 
    {
        pthread_mutex_lock(&g_uart_mutex);
        int current_fd = g_uart_fd;//获取串口文件标识符
        pthread_mutex_unlock(&g_uart_mutex);

        if (current_fd < 0) { sleep(1); continue; }

        // 阻塞读串口时不持有锁，否则网页无法保存新串口路径
        int n = read(current_fd, buf, sizeof(buf));//串口缓冲区有几个字节

        if (n < 0)
        {
            if (errno == EINTR) continue;
            gateway_log_event("ERROR", "backend", "serial_error", NULL, -1, -1, "协调器串口读取失败：%s", strerror(errno));
            perror("⚠️ [UART] 协调器串口读取失败");
            pthread_mutex_lock(&g_uart_mutex);
            if (g_uart_fd == current_fd)
            {
                g_uart_fd = -1;
                close(current_fd);
            }
            pthread_mutex_unlock(&g_uart_mutex);
            sleep_microseconds(100000U);
            continue;
        }
        if (n == 0)
        {
            v2_gateway_tick();
            gx_retry_tick();
            /* 串口被拔出后避免空转；协调器在线状态由
             * 最后一次 0x8A 应答的超时时间自动转为离线。 */
            sleep_microseconds(100000U);
            continue;
        }

        for (int i = 0; i < n; i++) 
        {
            // 驱动整个状态机运转！
            uart_parse_byte(&parser, buf[i]);
        }
        /* Parse all bytes already read before deciding that a fragment is lost. */
        v2_gateway_tick();
        gx_retry_tick();
    }
    return NULL;
}

/*********************************************************************
 * Mongoose 网络事件回调函数 
 *********************************************************************/
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) 
{
    // 如果是网络请求事件，直接扔给“路由器”
    if (ev == MG_EV_HTTP_MSG) 
    {
        route_http_requests(c, (struct mg_http_message *)ev_data);
    }
    
    // 如果是空闲轮询事件，依次去执行后台任务
    else if (ev == MG_EV_POLL) 
    {
        task_broadcast_ws(c);     // 任务1：推送网页数据
        task_dispatch_queries();  // 任务2：维护流量红绿灯
        join_tick(); /* Poll actual PermitJoin; manual close revokes only candidates. */
        manual_admission_tick();
        task_pending_signal();
        task_periodic_maintenance();  // 任务4：历史数据周期维护
        task_channel_migration(); // 任务5：查询并确认实际信道
    }
}

//出队列链表的函数，并且会把出队的短地址赋给out_short_addr
int dequeue_device(uint16_t *out_short_addr) 
{
    pthread_mutex_lock(&g_queue_mutex);
    if (g_queue_head == NULL) 
    {
        pthread_mutex_unlock(&g_queue_mutex);
        return 0; // 队列为空
    }
    ScanQueueNode_t* temp = g_queue_head;
    *out_short_addr = temp->short_addr;
    
    g_queue_head = g_queue_head->next;
    if (g_queue_head == NULL) {
        g_queue_tail = NULL;
    }
    free(temp);
    pthread_mutex_unlock(&g_queue_mutex);
    return 1; // 成功取出一个
}

//进入队列链表的函数
void enqueue_device(uint16_t short_addr) 
{
    ScanQueueNode_t* newNode = (ScanQueueNode_t*)malloc(sizeof(ScanQueueNode_t));
    if (newNode == NULL) 
    {
        printf("❌ [严重错误] 内存分配失败，无法将设备压入队列！\n");
        return; // 直接丢弃该任务，保住网关不死机
    }
    newNode->short_addr = short_addr;
    newNode->next = NULL;

    pthread_mutex_lock(&g_queue_mutex);
    if (g_queue_tail == NULL) 
    {
        g_queue_head = newNode;
        g_queue_tail = newNode;
    } 
    else {
        g_queue_tail->next = newNode;
        g_queue_tail = newNode;
    }
    pthread_mutex_unlock(&g_queue_mutex);
}


/*********************************************************************
 * 业务处理函数：处理 0x84 (Device Announce 候选)
 *********************************************************************/
void handle_cmd_84_device_join(unsigned char *payload, unsigned char len) 
{
    if (len < 10U) return;
    // 1. 提取 MAC 地址
    char mac_str[24] = {0};
    sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", 
            payload[0], payload[1], payload[2], payload[3], 
            payload[4], payload[5], payload[6], payload[7]);

    // 2. 提取短地址
    uint16_t short_addr = payload[8] | (payload[9] << 8);

    /* 短地址会被协议栈复用。收到带EUI64的入网通知后，先解除其他已
     * 撤销节点对这个短地址的旧绑定，避免以后按短地址误踢新节点。 */
    (void)release_revoked_short_for_other_mac(mac_str, short_addr);

    RevokedNodeRecord_t revoked;
    if (get_revoked_node_by_mac(mac_str, &revoked))
    {
        (void)update_revoked_node_seen(mac_str, short_addr, "pending");
        int submitted = submit_removed_device_leave(payload);
        gateway_log_event(submitted ? "INFO" : "WARN", "backend", "removal_retry", mac_str, short_addr, -1,
                          "撤销节点再次宣告，离网指令%s", submitted ? "已提交协调器，等待执行结果" : "串口提交失败");
        printf("[待离网拦截] MAC=%s | %s\n", mac_str,
               submitted ? "离网请求已提交串口" : "串口提交失败，保留离网任务");
        return;
    }

    // ==========================================
    // 新节点首次宣告也会建库；存在记录不等于曾经完成入网。
    // ==========================================
    if (is_device_in_db(mac_str)) 
    {
        if (device_announcement_is_reconnect(mac_str, short_addr))
            printf("♻️ [静默重连] 已确认设备重新宣告 MAC: %s, 当前短地址: 0x%04X\n", mac_str, short_addr);
        
        // 同步更新数据库里的状态为 online 和最新的短地址
        char short_addr_str[16];
        snprintf(short_addr_str, sizeof(short_addr_str), "%04X", short_addr);
        update_device_online(mac_str, short_addr_str);
        gateway_log_event("INFO", "coordinator", "node_announced", mac_str, short_addr, -1,
                          "收到设备宣告，更新网络地址为 0x%04X（宣告本身不代表应用确认）", short_addr);

        // 通知前端老设备已上线，把它点亮！
        char json_buf[256];
        snprintf(json_buf, sizeof(json_buf), 
                 "{\"type\":\"status_update\", \"mac\":\"%s\", \"status\":\"Online\",\"short_addr\":%u,\"last_active\":%lld}",
                 mac_str, (unsigned)short_addr, (long long)time(NULL));

        pthread_mutex_lock(&g_ws_mutex);
        strcpy(g_ws_msg_buffer, json_buf);
        queue_ws_event_locked();
        pthread_mutex_unlock(&g_ws_mutex);

        /* The coordinator owns descriptor/model discovery. A second standard
         * announcement must not add a parallel legacy Basic Model query.
         * Sampling parameters are requested by the node after HELLO. */

        return; 
    }
    /* The secured stack's Device Announce establishes a candidate only.
       A late unapproved announcement after manual close must be removed.
       Router descendants need not appear in our local child table. */
    accept_verified_scan_node(payload,0,-128);
}

/*********************************************************************
 * 业务处理函数：处理 0x85 (设备回复名字与信号)
 *********************************************************************/
void handle_cmd_85_device_info(unsigned char *payload, unsigned char len) 
{
    if (len < 4U || (unsigned)payload[3] + 4U > len) return;
    //提取短地址，信号强度，设备名称长度和设备名陈
    uint16_t short_addr = payload[0] | (payload[1] << 8);
    unsigned char name_len = payload[3];
    char device_name[64] = {0};
    
    if (name_len > 0 && name_len < 60) 
    {
        memcpy(device_name, &payload[4], name_len); 
        device_name[name_len] = '\0'; 
    } 
    else 
    {
        strcpy(device_name, "Unknown");
    }

    // 查映射表寻找 MAC
    char found_mac[24] = "Unknown-MAC";
    int found = 0; // 💡 新增：标记是否找到

    pthread_mutex_lock(&g_pending_mutex);
    for (int k = 0; k < g_pending_count; k++) 
    {
        if (g_pending_list[k].short_addr == short_addr) 
        {
            strcpy(found_mac, g_pending_list[k].mac_str);
            
            // 💡 方案B核心：数据落袋为安，死死捏在后端手里
            strcpy(g_pending_list[k].device_name, device_name);
            /* Resolve the name here; get current RSSI with a fresh probe.
             * Never promote a cached neighbor signal through a model reply. */
            if (g_scan_state == 1 && !g_pending_list[k].is_confirmed &&
                     !g_pending_list[k].signal_deadline_ms && signal_refresh_cursor < 0) {
                signal_refresh_cursor = k;
                signal_batch_manual = 0;
                signal_next_send_ms = 0;
            }
            
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_mutex);

    // 🛡️ 核心拦截：如果没找到（说明已经被大清洗踢掉了），直接丢弃报文，不麻烦前端！
    if (!found) 
    {
        // 💡 可能是盲加的设备发回来的名字！去数据库里反查一下它的 MAC
        char short_addr_str[16];
        snprintf(short_addr_str, sizeof(short_addr_str), "%04X", short_addr);
        char db_mac[24] = {0};
        
        if (get_mac_by_short_addr(short_addr_str, db_mac)) 
        {
            char model_name[64] = {0};
            char custom_name[128] = {0};
            char display_name[128] = {0};

            /* Repeated ZCL replies are not identity changes. Keep custom
             * names intact, and only write/log/publish a changed model. */
            if (!get_device_name_info(db_mac, model_name, sizeof(model_name),
                                      custom_name, sizeof(custom_name),
                                      display_name, sizeof(display_name)) ||
                strcmp(model_name, device_name) == 0)
                return;
            if (update_device_model_name(db_mac, device_name) != 0)
            {
                gateway_log_event("ERROR", "backend", "node_model_update_failed",
                                  db_mac, short_addr, -1, "节点型号更新失败");
                return;
            }

            if (get_device_name_info(db_mac,
                                     model_name,
                                     sizeof(model_name),
                                     custom_name,
                                     sizeof(custom_name),
                                     display_name,
                                     sizeof(display_name)))
            {
                printf("📝 [名称更新] MAC: %s, 真实型号已更新为: %s\n", db_mac, model_name);
                publish_name_update(db_mac, model_name, custom_name, display_name);
            }
            return; 
        }

        printf("👻 [拦截] 收到已清退或未知设备的 0x85 数据(短地址: 0x%04X)，直接丢弃。\n", short_addr);
        return; 
    }

    printf("[节点发现] 型号=%s | MAC=%s | 短地址=0x%04X | 名称已获取，等待信号探测\n",
           device_name, found_mac, short_addr);
    /* First frontend appearance waits for a valid RSSI probe reply. */
}

/*********************************************************************
 * 串口上行数据路由分发器
 *********************************************************************/
/* 0x9F v1: coordinator uptime, metadata only. Never update device state or DB
 * from diagnostic indications; JOIN_IND is not business authorization. */
static void handle_radio_diagnostic(const unsigned char *p,unsigned char len)
{
    char message[1024];RadioDiagLog log;
    if(!radio_diag_format(p,len,message,sizeof(message),&log)) {
        if(g_radio_diag_enabled)printf("[无线诊断] 无效v2报文：长度=%u\n",(unsigned)len);
        return;
    }
    /* Identity-bearing upload failures are normal operational logs. Only
     * verbose successes, MAC observations and polls require the opt-in. */
    if(!g_radio_diag_enabled && !((p[1]==RADIO_DIAG_SUBMIT ||
        p[1]==RADIO_DIAG_AF_RESULT) && p[18]))return;
    if(!g_radio_diag_enabled) {
        char *observations=strstr(message," | 累计观测轮询=");
        if(observations)*observations='\0';
    }
    if(p[1]==RADIO_DIAG_COVERAGE) {
        static uint8_t seen,flags;static uint32_t tick;
        int repeat=seen && flags==p[19] && log.tick>=tick;
        seen=1;flags=p[19];tick=log.tick;
        if(repeat)return;
    }
    printf("[无线诊断 +%.3fs] %s\n",log.tick/1000.0,message);
    if((p[1]==RADIO_DIAG_SUBMIT || p[1]==RADIO_DIAG_AF_RESULT) && p[18])
        upload_trace_dump(log.address,log.batch,1,0);
    if(log.persist) {
        char mac[24]={0};
        if(log.address<0xFFF8U)(void)lookup_mac_by_short(log.address,mac);
        gateway_log_event(log.level,"coordinator",log.event,mac[0]?mac:NULL,
            log.address<0xFFF8U?log.address:-1,log.batch?log.batch:-1LL,"%s",message);
    }
}
static void handle_admission_diagnostic(const unsigned char *p, unsigned char len)
{
    if(p && len && p[0]==3U){
        char text[900];
        if(ut_format(p,len,text,sizeof(text))){
            if(p[1]==UT_CAPABILITY){
                static uint8_t seen;static uint32_t tick;
                uint32_t current=ut_u32(p+2);int repeat=seen && current>=tick;
                seen=1;tick=current;if(repeat)return;
                printf("[上传链路] %s\n",text);return;
            }
            char mac[24]={0};(void)lookup_mac_by_short(ut_u16(p+6),mac);
            printf("[上传链路 协调器] MAC=%s | %s\n",mac[0]?mac:"未知",text);
        }
        return;
    }
    if(p && len && p[0]==2U){handle_radio_diagnostic(p,len);return;}
    if (len != 24U || p[0] != 1U) {
        printf("[协调器诊断] 无效报文：长度=%u\n", (unsigned)len);
        return;
    }
    /* Ignore verbose reply traces from coordinators not yet reflashed. */
    if (p[1] == 0x37U) return;
    char mac[24];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13]);
    unsigned short_addr = read_le16(p + 14);
    unsigned value = read_le32(p + 20);
    printf("[协调器诊断 +%.3fs] ", read_le32(p + 2) / 1000.0);
    switch (p[1]) {
        case 1:
            printf("允许加入 | 请求时长=%u秒 | 实际剩余=%u秒 | NLME状态=0x%02X | 计数读回=0x%02X | 最终状态=0x%02X\n",
                   p[16], p[17], p[18], p[19], value);
            break;
        case 2:
            printf("JOIN_IND | MAC=%s | 短地址=0x%04X | 重入类型=%u | 安全重入=%u | 能力=0x%02X | 允许加入标志=%u\n",
                   mac, short_addr, p[16], p[17], p[18], p[19]);
            break;
        case 3: {
            const char *state = p[16] == 0 ? "安全重入" :
                p[16] == 1 ? "初次非安全加入" :
                p[16] == 2 ? "离网" : p[16] == 3 ? "非安全重入" : "未知";
            printf("UPDATE_DEVICE | MAC=%s | 短地址=0x%04X | 设备状态=0x%02X（%s，不是成功/失败码）| 加密类型=%u\n",
                   mac, short_addr, p[16], state, p[17]);
            break;
        }
        case 4:
            printf("VERIFY_KEY 收到密钥验证指示 | MAC=%s | 密钥类型=%u | 加密类型=%u（尚不代表验证成功）\n",
                   mac, p[16], p[17]);
            break;
        case 5:
            printf("CONFIRM_KEY | MAC=%s | 状态=0x%02X | 密钥类型=%u | 加密类型=%u\n",
                   mac, p[16], p[17], p[18]);
            break;
        case 6:
            printf("NETWORK_STATUS | 短地址=0x%04X | 网络状态码=0x%02X\n", short_addr, p[16]);
            break;
        case 7:
            printf("DEVICE_ANNCE | MAC=%s | 短地址=0x%04X\n", mac, short_addr);
            break;
        case 8:
            printf("LEAVE_IND | MAC=%s | 短地址=0x%04X | rejoin=%u（协调器本地指示）\n", mac, short_addr, p[16]);
            break;
        case 9:
            printf("diagnostic protocol v1 | 固件诊断修订=%u（4为NWK入站计数诊断版）| 观察过滤器=%u | 续期定时器=%u | 离网过滤器=%u | 公告过滤器=%u\n",
                   value, p[16], p[17], p[18], p[19]);
            break;
        case 10:
            printf("TC策略 | 请求时长=%u秒 | 设置或读取状态=0x%02X | 策略读回状态=0x%02X | 来源命令=0x%02X | 读回策略=0x%08X\n",
                   p[16], p[17], p[18], p[19], value);
            break;
        case 11:
            printf("允许加入未提交 | 请求时长=%u秒 | 网络就绪=%u | 启动状态=0x%02X | 来源命令=0x%02X\n",
                   p[16], p[18], p[17], p[19]);
            gateway_log_event("WARN", "coordinator", "permit_join_blocked", NULL, -1, -1,
                              "允许加入未提交：时长=%u秒，网络就绪=%u，启动状态=0x%02X，来源命令=0x%02X",
                              p[16], p[18], p[17], p[19]);
            break;
        case 12:
            printf("REQUEST_KEY 收到密钥请求 | MAC=%s | 请求密钥类型=%u | 加密类型=%u\n",
                   mac, p[16], p[17]);
            break;
        case 13:
        case 14:
            printf("链路密钥记录（TC/APP共用表）| 阶段=%s | MAC=%s | 读取状态=0x%02X | 属性=0x%02X | %s计数=%u | 索引=%u（读取失败时属性/计数无效）\n",
                   p[17] == 0 ? "开放加入前" : "JOIN指示后任务读取",
                   mac, p[19], p[16], p[1] == 13 ? "入站" : "出站", value, p[18]);
            break;
        case 15:
            printf("加入前密钥表快照结束 | 停止索引=%u | 读取状态=0x%02X | 达到64项上限=%u（停止于首个读取失败，不保证覆盖所有条目）\n",
                   p[16], p[17], p[18]);
            break;
        case 16:
            printf("密钥状态检查队列满 | MAC=%s | 本次未读取\n", mac);
            break;
        case 17: {
            unsigned cluster = value & 0xffffU;
            const char *name = cluster == 0x0002U ? "NODE_DESC_REQ" :
                cluster == 0x8002U ? "NODE_DESC_RSP" : "DEVICE_ANNCE";
            printf("ZDO_RX %s | 源短地址=0x%04X | MAC=%s | 目标=0x%04X | 序号=%u | 安全状态=0x%02X | 长度=%u | 响应状态=0x%02X（0xFF表示未提供；只证明接收，不证明响应送达）\n",
                   name, short_addr, mac, value >> 16U, p[17], p[16], p[19], p[18]);
            break;
        }
        case 18: {
            const char *name = short_addr == 0x100fU ? "要求TC密钥交换" :
                short_addr == 0x100dU ? "TC密钥交换方式" :
                short_addr == 0x100bU ? "TC密钥交换尝试次数" :
                short_addr == 0x100cU ? "TC密钥交换最大次数" : "TC节点加入超时";
            printf("BDB %s | 属性=0x%04X | 阶段=%u | 读取状态=0x%02X | 缓冲区大小=%u | 值=%u（读取失败时值无效）\n",
                   name, short_addr, p[16], p[18], p[17], value);
            break;
        }
        case 19: {
            const char *name = short_addr == 0x00a0U ? "安全等级" :
                short_addr == 0x00a2U ? "当前网络密钥序号" :
                short_addr == 0x0404U ? "帧计数重置冷却配置（秒）" : "网络出站帧计数";
            printf("NWK %s | 属性=0x%04X | 阶段=%u | 读取状态=0x%02X | 值=%u（读取失败时值无效）\n",
                   name, short_addr, p[16], p[18], value);
            break;
        }
        case 20:
            printf("当前网络密钥查询 | 阶段=%u | 状态=0x%02X | 序号=%u（只检查是否可读取，不打印密钥）\n",
                   p[16], p[17], p[18]);
            break;
        case 21:
            printf("信任中心地址 | 阶段=%u | 读取状态=0x%02X | MAC=%s（读取失败时地址无效）\n",
                   p[16], p[17], mac);
            break;
        case 22:
        case 23:
        case 25: {
            const char *name = p[1] == 22 ? "当前入站帧计数" :
                p[1] == 23 ? "已持久化入站帧计数" : "冷却计时字段原始值";
            printf("NWK节点记录 | 阶段=%s | MAC=%s | 密钥序号=%u | 索引=%u | 读取状态=0x%02X | %s=%u\n",
                   p[16] == 0 ? "开放加入前" : "JOIN后任务读取",
                   mac, p[17], p[18], p[19], name, value);
            break;
        }
        case 24:
            printf("NWK计数表检查结束 | 阶段=%s | 目标MAC=%s（全零为全表）| 匹配记录=%u | 停止索引=%u | 读取状态=0x%02X | 达到128项上限=%u（仅覆盖已读索引；匹配为0不等于确认无记录）\n",
                   p[16] == 0 ? "开放加入前" : "JOIN后任务读取",
                   mac, value, p[18], p[17], p[19]);
            break;
        case 0x30:
            printf("TI允许加入请求 | 请求时长=%u秒 | 提交状态=0x%02X | 来源命令=0x%02X（0为启动或自动续期；提交成功不等于已开启）\n",
                   p[16], p[17], p[18]);
            gateway_log_event(p[17] ? "ERROR" : "INFO", "coordinator", "permit_join_submit", NULL, -1, -1,
                              "TI%s加入请求%s：时长=%u秒，状态=0x%02X，来源命令=0x%02X（仅提交结果）",
                              p[16] ? "开放" : "关闭", p[17] ? "提交失败" : "已提交", p[16], p[17], p[18]);
            break;
        case 0x31:
            printf("TI允许加入状态 | 协议栈%s节点加入 | 本次窗口时长=%u秒（状态变化通知，不是剩余时间读回）\n",
                   p[16] ? "已允许" : "已关闭", p[16]);
            gateway_log_event("INFO", "coordinator", "permit_join_state", NULL, -1, -1,
                              "TI协议栈%s节点加入，窗口时长=%u秒", p[16] ? "已允许" : "已关闭", p[16]);
            break;
        case 0x32:
            printf("TI允许加入管理响应 | 源短地址=0x%04X | 状态=0x%02X\n", short_addr, p[16]);
            gateway_log_event(p[16] ? "ERROR" : "INFO", "coordinator", "permit_join_response", NULL, -1, -1,
                              "TI允许加入管理响应：源短地址=0x%04X，状态=0x%02X", short_addr, p[16]);
            break;
        case 0x33:
            printf("TI离网请求 | MAC=%s | 短地址=0x%04X | 提交状态=0x%02X（不是删除完成）\n", mac, short_addr, p[16]);
            gateway_log_event(p[16] ? "WARN" : "INFO", "coordinator", "leave_submit", mac, short_addr, -1,
                              "TI离网请求提交状态=0x%02X；等待协议栈完成和持久化校验", p[16]);
            break;
        case 0x34:
            printf("TI离网处理结果 | MAC=%s | 依据=%s | 状态=0x%02X | TC=0x%02X | NV=0x%02X（不代表节点已完成本地擦除）\n",
                   mac, p[16] == 1 ? "子节点Leave确认" : "远端Leave通知", p[17], p[18], p[19]);
            gateway_log_event(p[17] || p[18] || p[19] ? "WARN" : "INFO", "coordinator", "leave_cleanup", mac, short_addr, -1,
                              "TI%s：状态=0x%02X，TC=0x%02X，持久化校验=0x%02X",
                              p[16] == 1 ? "子节点Leave确认" : "远端Leave通知", p[17], p[18], p[19]);
            break;
        case 0x35:
            printf("TI离网等待超时 | MAC=%s | 短地址=0x%04X | 保留删除任务，等待节点活动或迟到确认\n", mac, short_addr);
            gateway_log_event("WARN", "coordinator", "leave_timeout", mac, short_addr, -1,
                              "TI离网等待超时；未报告删除成功，仍保留任务");
            break;
        case 0x36:
            printf("上传回复失败 | 短地址=0x%04X | 类型=%s | 批次/窗口=旧版诊断未提供 | 阶段=%s | 状态=%s（0x%02X）| 耗时=%u ms\n",
                   short_addr,p[16] ? "旧版诊断未提供" : radio_ack_kind(p[18]),
                   p[16] ? "AF发送结果（可能包含MAC错误）" : "协议栈提交",radio_ti_status(p[17]),p[17],value);
            gateway_log_event("WARN", "coordinator", "upload_ack_failure", NULL, short_addr, -1,
                              "上传回复失败 | 类型=%s | 批次/窗口=旧版诊断未提供 | 阶段=%s | 状态=%s（0x%02X）| 耗时=%u ms；未单独观测MAC轮询或发射过程",
                              p[16] ? "旧版诊断未提供" : radio_ack_kind(p[18]),
                              p[16] ? "AF发送结果" : "协议栈提交",radio_ti_status(p[17]),p[17],value);
            break;
        case 0x37: {
            const char *phase=p[17]==0?"投递失败，延迟重试":p[17]==1?"投递尝试":p[17]==2?"无线投递成功":
                p[17]==4?"节点已应用当前配置，停止后续重试":
                p[17]==5?"迟到的无线结果；节点已应用，不再重试":"保存最新命令";
            printf("控制命令重发 | MAC=%s | 短地址=0x%04X | %s | 次数=%u | 状态=0x%02X | %s=%u\n",
                   mac,short_addr,phase,p[18],p[19],p[17]==0?"下次延迟ms":"配置版本",value);
            gateway_log_event(p[17]==0?"WARN":"INFO","coordinator","control_retry",mac,short_addr,-1,
                              "%s；次数=%u，状态=0x%02X，%s=%u",phase,p[18],p[19],p[17]==0?"下次延迟ms":"配置版本",value);
            break;
        }
        case V2_WINDOW_QUERY_COALESCED_EVENT:
            printf("[窗口回复合并] MAC=%s | 短地址=0x%04X | 批次=%u | 窗口=%u | 查询序号=%u | 沿用在途AF事务=%u\n",
                   mac,short_addr,value,(unsigned)read_le16(p+18)+1U,p[16],p[17]);
            break;
        case 0xff:
            printf("协调器UART接收溢出或读取异常 | 计数=%u | 已丢弃不完整接收流并重置帧解析器\n", value);
            break;
        default:
            printf("未知事件=0x%02X\n", p[1]);
            break;
    }
    fflush(stdout);
}

#include "comm_test_gateway.h"
#include "admission_delivery.h"
#include "gateway_exchange_backend.h"

void dispatch_uart_command(unsigned char cmd, unsigned char *payload, unsigned char len) 
{
    // 判断最高位是否为1 (是否为协调器发来的上报/应答)
    if ((cmd & 0x80) == 0) return; 
    if(payload && len>=2U)upload_trace_packet(UT_HOST_RX,read_le16(payload),cmd,payload+2,len-2U);
    trace_config_uart(cmd, payload, len);

    /* 节点上行意味着睡眠终端此刻已经唤醒。若它处于待移除表，趁接收
     * 窗口重新发送离网指令；3秒节流避免振动分片造成命令风暴。 */
    if (len >= 2U &&
        (cmd == 0x86U || cmd == 0x87U || cmd == 0x88U || cmd == 0x89U ||
         cmd == GW_RSP_MONITOR_STATE ||
         cmd == GW_RSP_VIBRATION_CHUNK || cmd == GW_RSP_VIBRATION_SUMMARY ||
         cmd == 0x85U || cmd == 0x9BU || cmd == CONTROL_STATUS_CMD || cmd == SCALAR_MANIFEST_CMD || cmd == GW_RSP_CONFIG_QUERY ||
         cmd == V2_BATCH || cmd == V2_BLOCK || cmd == V2_DATA ||
         cmd == V2_QUERY || cmd == GX_UART_UP))
    {
        if (retry_removed_device_by_short(read_le16(payload), cmd)) {
            if (cmd == GW_RSP_CONFIG_QUERY)
                config_diag_print(mg_millis(), "DROP99 | 短地址=0x%04X | 处于待移除处理，原有流程不回复采样配置", read_le16(payload));
            return;
        }
    }

    /* 温度、磁场和声级处理函数会自行刷新；上传状态以及V2阶段报文也
     * 表示节点正在正常通信。避开每个0x94数据分片都写SQLite。 */
    if (len >= 2U &&
        (cmd == GW_RSP_MONITOR_STATE || cmd == SCALAR_MANIFEST_CMD || cmd == V2_BATCH ||
         cmd == V2_BLOCK || cmd == V2_QUERY))
        refresh_device_activity_by_short(read_le16(payload));

    /* A secured wireless member is not yet a user-authorized sensor. Accept
     * discovery/status traffic, but never admit pending scalar/raw batches. */
    if(len>=2U && ((cmd>=0x86U && cmd<=0x89U) || cmd==0x8BU || cmd==0x8CU ||
        cmd==SCALAR_MANIFEST_CMD || (cmd>=V2_BATCH && cmd<=V2_QUERY))) {
        char mac[24];
        if(!lookup_mac_by_short(read_le16(payload),mac) || !device_is_authorized(mac))return;
    }

    switch (cmd) 
    {
        case GX_UART_UP:
            handle_gateway_exchange(payload,len);
            break;
        case SCALAR_MANIFEST_CMD:
            handle_scalar_manifest(payload,len);
            break;
        case CONTROL_STATUS_CMD:
            handle_control_status(payload,len);
            if (len==12U && control_status_valid(payload+2,CONTROL_STATUS_SIZE))
                channel_migration_note_node_awake(read_le16(payload));
            break;
        case GW_RSP_RECOVERY_ACTIVITY: /* Fresh hello, not cached discovery. */
            if (len == 10U)
            {
                char mac[24];
                snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                         payload[0], payload[1], payload[2], payload[3],
                         payload[4], payload[5], payload[6], payload[7]);
                uint16_t address = read_le16(payload + 8);
                gateway_log_event("INFO", "coordinator", "recovery_activity", mac, address, -1,
                                  "协调器上报节点恢复活动，网络地址=0x%04X（本报文不代表节点已收到握手确认）", address);
                (void)release_revoked_short_for_other_mac(mac, address);
                if (get_revoked_node_by_mac(mac, NULL))
                {
                    (void)update_revoked_node_seen(mac, address, "pending");
                    (void)retry_removed_device_by_short(address, cmd);
                }
            }
            break;
        //在扫描中，设备入网的报文处理
        case 0xA1: join_observe(payload,len); break;
        case 0xA2: case 0xA3: handle_interview(cmd,payload,len); break;
        case 0x84:
            handle_cmd_84_device_join(payload, len);
            break;
        //在扫描中，设备入网初步入网后传来的设备信息  
        case 0x85:
            handle_cmd_85_device_info(payload, len);
            break;
        //设备正式入网后，上传的温度数据
        case 0x86:
            handle_cmd_86_temperature(payload, len);
            break;
        //设备正式入网后，上传的磁力计三轴数据
        case 0x87:
            handle_cmd_87_magnetic(payload, len);
            break;
        //设备正式入网后，上传数字/超声麦克风的一秒声级
        case 0x88:
            handle_cmd_88_89_sound(payload, len, 1);
            break;
        case 0x89:
            handle_cmd_88_89_sound(payload, len, 0);
            break;
        //协调器当前PAN ID、实际信道和网络状态
        case 0x8A:
            handle_cmd_8a_network_info(payload, len);
            break;
        case 0x97:
            /* 兼容仍在发送旧调试帧的协调器固件，默认静默丢弃。 */
            break;
        case 0x98:
            if (len >= 4U)
            {
                uint16_t ready_pan_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
                unsigned char source = len >= 5U ? payload[4] : GW_NETWORK_SOURCE_UNKNOWN;
                const char *source_text = source == GW_NETWORK_SOURCE_RESTORED
                    ? "旧网络恢复" : (source == GW_NETWORK_SOURCE_AUTO_FORM
                    ? "自动扫描建网" : "未知来源");
                printf("✅ [Zigbee网络就绪] 来源=%s | PAN ID: 0x%04X | 当前信道: %u | 状态: 0x%02X\n",
                       source_text, ready_pan_id, payload[2], payload[3]);
                if (payload[3] == 0U && payload[2] >= 11U && payload[2] <= 26U)
                    (void)save_zigbee_network_state(ready_pan_id, payload[2],
                        source == GW_NETWORK_SOURCE_RESTORED ? "restored" :
                        (source == GW_NETWORK_SOURCE_AUTO_FORM ? "auto_scan" : "unknown"));
            }
            else
            {
                printf("⚠️ [Zigbee建网] 0x98报文长度不足：%u\n", len);
            }
            break;
        case GW_RSP_NETWORK_RESTORE:
            handle_cmd_9a_network_restore(payload, len);
            break;
        case GW_RSP_CONFIG_QUERY:
            handle_cmd_99_config_query(payload, len);
            if (len >= 2U)
                channel_migration_note_node_awake(read_le16(payload));
            break;
        case V2_BATCH:
        case V2_BLOCK:
        case V2_DATA:
        case V2_QUERY:
            handle_vibration_v2(cmd,payload,len);
            break;
        case 0x9BU:
            admission_probe_reply(payload, len);
            handle_pending_signal(payload, len);
            break;
        case COMM_TEST_REPORT:
            if (len == COMM_TEST_REPORT_LEN && payload[2] == COMM_TEST_VERSION &&
                retry_removed_device_by_short(read_le16(payload), cmd)) break;
            handle_comm_test_report(payload,len);
            break;
        case 0x9CU:
            if (len == 9U)
                printf("%s [未授权离网] MAC=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X | 状态=0x%02X（Leave请求%s）\n",
                    payload[8] == 0U ? "✅" : "⚠️",
                    payload[0], payload[1], payload[2], payload[3],
                    payload[4], payload[5], payload[6], payload[7], payload[8],
                    payload[8] == 0U ? "已受理" : "提交失败");
            break;
        case 0x9DU:
            handle_scan_neighbor_entry(payload, len);
            break;
        case 0x9EU:
            handle_scan_neighbor_done(payload, len);
            break;
        case GW_RSP_VIBRATION_CHUNK:
            handle_cmd_8b_vibration_chunk(payload, len);
            break;
        case GW_RSP_VIBRATION_SUMMARY:
            handle_cmd_8c_vibration_summary(payload, len);
            break;
        case GW_RSP_MONITOR_STATE:
            handle_cmd_8d_monitor_state(payload, len);
            break;
        case GW_RSP_COMMAND_QUEUED:
            if (len >= 4U && payload[3] != 0U)
            {
                uint16_t short_addr = read_le16(payload);
                char mac[24] = {0}; (void)lookup_mac_by_short(short_addr, mac);
                gateway_log_event("ERROR", "coordinator", "command_queue_failed", mac, short_addr, -1,
                                  "命令 0x%02X 入队失败，状态 0x%02X", payload[2], payload[3]);
                printf("❌ [协调器入队失败] 短地址=0x%04X | 命令=0x%02X（%s）| 状态=0x%02X\n",
                       short_addr, payload[2],
                       downlink_command_description(payload[2]), payload[3]);
            }
            break;
        case GW_RSP_DEVICE_LEFT:
            if (len >= 10U)
            {
                char mac[24];
                snprintf(mac, sizeof(mac),
                         "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                         payload[0], payload[1], payload[2], payload[3],
                         payload[4], payload[5], payload[6], payload[7]);
                uint16_t short_addr = read_le16(payload + 8);
                gateway_log_event("INFO", "coordinator", "node_left", mac, short_addr, -1, "协调器报告节点离网");
                printf("[Zigbee离网] 协调器报告节点离开: MAC=%s, 短地址=0x%04X\n",
                       mac, short_addr);
                if (len >= 13U)
                {
                    printf("[Zigbee清理] MAC=%s | TC链路密钥=0x%02X | "
                           "应用链路密钥=0x%02X | 持久化=%s\n",
                           mac, payload[10], payload[11],
                           payload[12] == 0U ? "成功" : (payload[12] == 0xffU ? "未确认" : "失败"));
                }
                /* Only the explicit tracked-removal completion may retire
                 * a durable task. Generic Leave/rejoin events are not ACKs. */
                if (len >= 14U && payload[13] == 1U &&
                    payload[10] == 0U && payload[11] == 0U && payload[12] == 0U)
                {
                    pthread_mutex_lock(&g_pending_mutex);
                    for (int k = 0; k < g_pending_count; ++k)
                        if (strcasecmp(g_pending_list[k].mac_str, mac) == 0)
                            g_pending_list[k].leave_completed = 1;
                    pthread_mutex_unlock(&g_pending_mutex);
                    if (get_revoked_node_by_mac(mac, NULL))
                        (void)clear_revoked_node(mac);
                    gateway_log_event("INFO", "coordinator", "removal_completed", mac, short_addr, -1, "收到明确的清退完成结果，离网任务已结束");
                }
                else if (get_revoked_node_by_mac(mac, NULL))
                    printf("[设备移除] MAC=%s | 尚未确认清理成功，保留离网任务\n", mac);
                if (is_device_in_db(mac))
                {
                    update_device_offline(mac);
                    pthread_mutex_lock(&g_ws_mutex);
                    snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer),
                             "{\"type\":\"status_update\",\"mac\":\"%s\",\"status\":\"offline\"}",
                             mac);
                    queue_ws_event_locked();
                    pthread_mutex_unlock(&g_ws_mutex);
                }
            }
            break;
        case GW_RSP_AF_CONFIRM:
            if (len == GW_ADMISSION_RESULT_SIZE && payload[2] == GW_CMD_MONITOR_CONTROL) {
                admission_delivery_result(payload);
                break;
            }
            if (len >= 5U && payload[3] != 0U)
            {
                uint16_t short_addr = read_le16(payload);
                char mac[24] = {0}; (void)lookup_mac_by_short(short_addr, mac);
                gateway_log_event("ERROR", "coordinator", "delivery_failed", mac, short_addr, -1,
                                  "命令 0x%02X 无线投递失败，状态 0x%02X，事务号 %u", payload[2], payload[3], payload[4]);
                printf("❌ [Zigbee无线投递失败] 短地址=0x%04X | 命令=0x%02X（%s）| "
                       "状态=0x%02X | 事务号=%u\n",
                       short_addr, payload[2], downlink_command_description(payload[2]),
                       payload[3], payload[4]);
            }
            break;
        case GW_RSP_AF_RX_TRACE:
            /* 原始帧追踪保留在协议中，默认终端不输出。 */
            break;
        case GW_RSP_CHANNEL_CONFIG:
            if (len >= 4U)
            {
                unsigned char phase = len >= 5U ? payload[4] : 0U;
                printf("[Zigbee信道配置] 策略=%s, 目标信道=%u, "
                       "状态=0x%02X（%s）, 原信道=%u, 阶段=0x%02X。\n",
                       payload[0] == 0U ? "自动" : "手动",
                       payload[1], payload[2],
                       zigbee_channel_status_description(payload[2]), payload[3], phase);

                if (phase == GW_CHANNEL_PHASE_PREPARE && len >= 7U)
                {
                    uint16_t prepared_short = read_le16(payload + 5U);
                    unsigned char prepared_count = 0U;
                    unsigned char node_count = 0U;
                    int operation_matches = 0;
                    pthread_mutex_lock(&g_channel_operation_mutex);
                    operation_matches =
                        g_channel_operation.phase == CHANNEL_OPERATION_PENDING &&
                        g_channel_operation.waiting_for_nodes &&
                        g_channel_operation.strategy == payload[0] &&
                        g_channel_operation.target_channel == payload[1];
                    if (operation_matches && payload[2] == 0U)
                    {
                        for (unsigned char i = 0U;
                             i < g_channel_operation.node_count; ++i)
                        {
                            ChannelMigrationNode_t *node =
                                &g_channel_operation.nodes[i];
                            if (node->short_addr == prepared_short &&
                                node->state == CHANNEL_NODE_SENT)
                            {
                                node->state = CHANNEL_NODE_PREPARED;
                                g_channel_operation.prepared_count++;
                                break;
                            }
                        }
                        snprintf(g_channel_operation.message,
                                 sizeof(g_channel_operation.message),
                                 "已通知%u/%u个节点，等待其余节点唤醒",
                                 g_channel_operation.prepared_count,
                                 g_channel_operation.node_count);
                    }
                    else if (operation_matches)
                    {
                        for (unsigned char i = 0U;
                             i < g_channel_operation.node_count; ++i)
                        {
                            ChannelMigrationNode_t *node =
                                &g_channel_operation.nodes[i];
                            if (node->short_addr == prepared_short &&
                                node->state == CHANNEL_NODE_SENT)
                            {
                                node->state = CHANNEL_NODE_WAITING;
                                break;
                            }
                        }
                        snprintf(g_channel_operation.message,
                                 sizeof(g_channel_operation.message),
                                 "节点0x%04X通知失败，下次唤醒时重试",
                                 prepared_short);
                    }
                    prepared_count = g_channel_operation.prepared_count;
                    node_count = g_channel_operation.node_count;
                    pthread_mutex_unlock(&g_channel_operation_mutex);
                    if (operation_matches) persist_channel_operation_snapshot();

                    if (operation_matches && payload[2] != 0U)
                    {
                        char retry_message[96];
                        snprintf(retry_message, sizeof(retry_message),
                                 "节点0x%04X通知失败，下次唤醒时重试",
                                 prepared_short);
                        publish_channel_operation_event(
                            CHANNEL_OPERATION_PENDING, payload[1], payload[3],
                            payload[2], prepared_count, node_count,
                            retry_message);
                        printf("⚠️ [Zigbee信道迁移] 节点0x%04X准备失败，下次唤醒重试：%s。\n",
                               prepared_short,
                               zigbee_channel_status_description(payload[2]));
                    }
                    else if (operation_matches)
                    {
                        char progress[96];
                        snprintf(progress, sizeof(progress),
                                 "已通知%u/%u个节点，等待其余节点唤醒",
                                 prepared_count, node_count);
                        publish_channel_operation_event(
                            CHANNEL_OPERATION_PENDING, payload[1], payload[3],
                            payload[2], prepared_count, node_count, progress);
                        printf("[Zigbee信道迁移] 节点0x%04X准备完成，进度=%u/%u。\n",
                               prepared_short, prepared_count, node_count);
                        channel_migration_commit_if_ready();
                    }
                    break;
                }

                pthread_mutex_lock(&g_channel_operation_mutex);
                int operation_matches =
                    g_channel_operation.phase == CHANNEL_OPERATION_PENDING &&
                    g_channel_operation.strategy == payload[0] &&
                    g_channel_operation.target_channel == payload[1];
                if (operation_matches)
                {
                    g_channel_operation.current_channel = payload[3];
                    g_channel_operation.coordinator_status = payload[2];
                }
                pthread_mutex_unlock(&g_channel_operation_mutex);
                if (operation_matches) persist_channel_operation_snapshot();

                if (operation_matches && payload[2] != 0U)
                {
                    finish_channel_operation(CHANNEL_OPERATION_FAILED,
                                             payload[3], payload[2],
                                             zigbee_channel_status_description(payload[2]));
                }
                request_network_info();
            }
            break;
        case 0x81:
            if (len > 0U && payload[0] != 0U)
                printf("❌ [节点授权] 协调器白名单写入失败，状态=0x%02X\n",
                       payload[0]);
            break;
            
        case 0x9F:
            handle_admission_diagnostic(payload, len);
            break;

        case 0x83:
            if (len < 1)
            {
                printf("⚠️ [扫描节点] 协调器扫描应答长度不足：%d\n", len);
            }
            else if (payload[0] == 0x00)
            {
                printf("✅ [扫描节点] 协调器开放网络请求已接受（实际状态以协议栈通知为准）\n");
            }
            else
            {
                printf("❌ [扫描节点] 协调器开放网络失败，状态=0x%02X\n",
                       payload[0]);
            }
            break;
            
        default:
            printf("⚠️ [网关警告] 收到未知的上行指令: 0x%02X\n", cmd);
            break;
    }
}

// 保存协调器的实际网络信息，不将自动选择的信道写成固定配置
void handle_cmd_8a_network_info(unsigned char *payload, unsigned char len)
{
    if (len < 4)
    {
        printf("⚠️ [Zigbee网络] 0x8A报文长度不足：%d\n", len);
        return;
    }
    admission_delivery_capabilities(len >= 10U ? payload[9] : 0U);
    /* Reapply the observational switch after a coordinator reboot/reconnect.
     * This command does not change radio, queue or device configuration. */
    request_radio_diagnostics();

    unsigned char restore_changed = 0U;
    unsigned char incoming_state = len >= 9U ? payload[8]
        : (payload[3] == 9U ? GW_NETWORK_STATE_READY
                            : GW_NETWORK_STATE_NOT_READY);
    unsigned char incoming_source = len >= 6U
        ? payload[5] : GW_NETWORK_SOURCE_UNKNOWN;
    unsigned char incoming_startup_status = len >= 5U ? payload[4] : 0xFFU;
    pthread_mutex_lock(&g_network_info_mutex);
    int network_changed = !g_has_network_info ||
        g_network_pan_id != ((uint16_t)payload[0] | ((uint16_t)payload[1] << 8)) ||
        g_network_channel != payload[2] ||
        g_network_device_state != payload[3] ||
        g_network_state != incoming_state ||
        g_network_startup_source != incoming_source ||
        g_network_startup_status != incoming_startup_status;
    g_network_pan_id = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    g_network_channel = payload[2];
    g_network_device_state = payload[3];
    g_network_state = incoming_state;
    g_network_startup_source = incoming_source;
    g_network_startup_status = incoming_startup_status;
    if (len >= 8U && (g_network_restore_result != payload[6] ||
                      g_network_restore_status != payload[7]))
    {
        g_network_restore_result = payload[6];
        g_network_restore_status = payload[7];
        restore_changed = payload[6] != 0xFFU;
    }
    g_has_network_info = 1;
    g_network_last_seen_monotonic_ms = coordinator_clock_ms(CLOCK_MONOTONIC);
    g_network_last_seen_epoch_ms = coordinator_clock_ms(CLOCK_REALTIME);
    pthread_mutex_unlock(&g_network_info_mutex);

    /* 恢复成功时由下面的0x8A网络摘要统一打印，避免同一报文连续输出
     * “恢复结果”和“网络信息”两行。无数据或恢复失败仍保留原因日志。 */
    if (network_changed) gateway_log_event(incoming_state == GW_NETWORK_STATE_READY ? "INFO" : "WARN",
        "coordinator", "network_state", NULL, -1, -1,
        "网络状态=%u，PAN=0x%04X，信道=%u，启动来源=%u（旧网络恢复=%u），建网状态=0x%02X",
        incoming_state, (unsigned)(payload[0] | (payload[1] << 8)), payload[2], incoming_source,
        GW_NETWORK_SOURCE_RESTORED, incoming_startup_status);
    if (restore_changed && payload[6] != GW_RESTORE_SUCCESS)
    {
        unsigned char restore_payload[5] = {
            payload[6], payload[7], payload[0], payload[1], payload[2]
        };
        handle_cmd_9a_network_restore(restore_payload, sizeof(restore_payload));
    }

    if (g_network_state == GW_NETWORK_STATE_READY &&
        (len < 5U || payload[4] == 0U) &&
        payload[2] >= 11U && payload[2] <= 26U)
    {
        const char *source = g_network_startup_source == GW_NETWORK_SOURCE_RESTORED
            ? "restored" : (g_network_startup_source == GW_NETWORK_SOURCE_AUTO_FORM
            ? "auto_scan" : "unknown");
        (void)save_zigbee_network_state(g_network_pan_id, g_network_channel, source);
    }

    if (network_changed && restore_changed && len >= 8U &&
        payload[6] == GW_RESTORE_SUCCESS &&
        payload[7] == 0U)
    {
        printf("✅ [Zigbee网络] 协调器已恢复原网络 | PAN ID=0x%04X | "
               "信道=%u | 设备状态: %u | 建网状态: 0x%02X\n",
               g_network_pan_id, g_network_channel,
               g_network_device_state, payload[4]);
    }
    else if (network_changed && g_network_state == GW_NETWORK_STATE_RESTORING)
    {
        printf("🔄 [Zigbee建网] 协调器正在恢复持久化网络。\n");
    }
    else if (network_changed && g_network_state == GW_NETWORK_STATE_SCANNING)
    {
        printf("🔎 [Zigbee建网] 协调器正在扫描11～26信道。\n");
    }
    else if (network_changed && g_network_state == GW_NETWORK_STATE_FORMING)
    {
        printf("🛠️ [Zigbee建网] 协调器正在建立新网络。\n");
    }
    else if (network_changed && g_network_state == GW_NETWORK_STATE_ERROR)
    {
        printf("❌ [Zigbee建网] 协调器建网失败，状态=0x%02X。\n",
               incoming_startup_status);
    }
    else if (network_changed && len >= 5U)
    {
        printf("📡 [Zigbee网络] PAN ID: 0x%04X | 当前信道: %u | 设备状态: %u | 建网状态: 0x%02X\n",
               g_network_pan_id, g_network_channel, g_network_device_state, payload[4]);
    }
    else if (network_changed)
    {
        printf("📡 [Zigbee网络] PAN ID: 0x%04X | 当前信道: %u | 设备状态: %u\n",
               g_network_pan_id, g_network_channel, g_network_device_state);
    }

    pthread_mutex_lock(&g_channel_operation_mutex);
    int pending = g_channel_operation.phase == CHANNEL_OPERATION_PENDING;
    unsigned char strategy = g_channel_operation.strategy;
    unsigned char target = g_channel_operation.target_channel;
    unsigned char coordinator_status = g_channel_operation.coordinator_status;
    unsigned char commit_sent = g_channel_operation.commit_sent;
    if (pending) g_channel_operation.current_channel = payload[2];
    pthread_mutex_unlock(&g_channel_operation_mutex);

    if (pending && (coordinator_status == 0U || commit_sent) &&
        ((strategy == 0U && payload[2] >= 11U && payload[2] <= 26U) ||
         (strategy == 1U && payload[2] == target)))
    {
        finish_channel_operation(CHANNEL_OPERATION_SUCCESS, payload[2], 0U,
                                 strategy == 0U
                                    ? "自动信道策略已应用"
                                    : "信道切换成功");
        printf("✅ [Zigbee信道配置] 已由实时网络信息确认，当前实际信道=%u。\n",
               payload[2]);
    }
}

static void reset_vibration_assembly(VibrationAssembly_t *slot)
{
    free(slot->packet_bitmap);
    free(slot->samples);
    memset(slot, 0, sizeof(*slot));
}

static VibrationAssembly_t *find_vibration_assembly(uint16_t short_addr,
                                                    uint32_t batch_id,
                                                    uint16_t packet_count)
{
    VibrationAssembly_t *free_slot = NULL;
    VibrationAssembly_t *oldest_slot = NULL;
    for (size_t i = 0; i < VIBRATION_ASSEMBLY_SLOTS; i++)
    {
        VibrationAssembly_t *slot = &g_vibration_assemblies[i];
        if (slot->active && slot->short_addr == short_addr && slot->batch_id == batch_id)
        {
            return slot->packet_count == packet_count ? slot : NULL;
        }
        if (!slot->active && !free_slot) free_slot = slot;
        if (slot->active && (!oldest_slot || slot->last_update < oldest_slot->last_update))
        {
            oldest_slot = slot;
        }
    }

    VibrationAssembly_t *slot = free_slot ? free_slot : oldest_slot;
    if (!slot || packet_count == 0) return NULL;
    if (slot->active) reset_vibration_assembly(slot);

    size_t max_samples = (size_t)packet_count * VIBRATION_SAMPLES_PER_PACKET;
    if (max_samples > VIBRATION_MAX_SAMPLE_COUNT) return NULL;
    slot->packet_bitmap = calloc((packet_count + 7U) / 8U, 1U);
    slot->samples = calloc(max_samples * 3U, sizeof(int16_t));
    if (!slot->packet_bitmap || !slot->samples)
    {
        reset_vibration_assembly(slot);
        return NULL;
    }
    slot->active = 1;
    slot->short_addr = short_addr;
    slot->batch_id = batch_id;
    slot->packet_count = packet_count;
    slot->last_update = time(NULL);
    return slot;
}

static void finish_vibration_assembly(VibrationAssembly_t *slot)
{
    if (!slot || !slot->active || !slot->summary_received ||
        slot->received_packets != slot->packet_count ||
        slot->received_samples != slot->sample_count ||
        slot->sample_count == 0 || slot->sample_count > VIBRATION_MAX_SAMPLE_COUNT)
    {
        return;
    }

    char mac[24] = {0};
    if (!lookup_mac_by_short(slot->short_addr, mac))
    {
        printf("⚠️ [振动数据] 找不到短地址 0x%04X 对应的节点。\n", slot->short_addr);
        reset_vibration_assembly(slot);
        return;
    }

    size_t sample_bytes = (size_t)slot->sample_count * 3U * sizeof(int16_t);
    if (save_vibration_batch(mac, slot->batch_id, slot->sample_rate_hz,
                             slot->sample_count, slot->samples, sample_bytes,
                             slot->features) == 0)
    {
        update_device_collection_batch_state(mac, "complete", 1, slot->batch_id);
        publish_collection_data_ready_event(mac, slot->batch_id, 1);
        printf("✅ [振动数据] %s 批次 %u 已完整入库：%u 点，%u Hz。\n",
               mac, slot->batch_id, slot->sample_count, slot->sample_rate_hz);
    }
    else
    {
        update_device_collection_batch_state(mac, "error", 0, slot->batch_id);
        printf("❌ [振动数据] %s 批次 %u 数据库写入失败。\n", mac, slot->batch_id);
    }
    reset_vibration_assembly(slot);
}

void handle_cmd_8b_vibration_chunk(unsigned char *payload, unsigned char len)
{
    /* short(2) + collection(4) + index(2) + count(2) + rate(2) + samples(1) */
    if (len < 13U) return;
    uint16_t short_addr = read_le16(payload);
    uint32_t batch_id = read_le32(payload + 2);
    uint16_t packet_index = read_le16(payload + 6);
    uint16_t packet_count = read_le16(payload + 8);
    uint16_t sample_rate = read_le16(payload + 10);
    unsigned char samples_in_packet = payload[12];
    size_t data_bytes = (size_t)samples_in_packet * 3U * sizeof(int16_t);
    if (samples_in_packet == 0 || samples_in_packet > VIBRATION_SAMPLES_PER_PACKET ||
        packet_index >= packet_count || (size_t)len != 13U + data_bytes)
    {
        printf("⚠️ [振动数据] 非法分片：批次%u 序号%u/%u 长度%u。\n",
               batch_id, packet_index, packet_count, len);
        return;
    }

    pthread_mutex_lock(&g_vibration_mutex);
    VibrationAssembly_t *slot = find_vibration_assembly(short_addr, batch_id, packet_count);
    if (slot && slot->packet_count == packet_count)
    {
        size_t byte_index = packet_index / 8U;
        unsigned char bit = (unsigned char)(1U << (packet_index % 8U));
        if ((slot->packet_bitmap[byte_index] & bit) == 0)
        {
            size_t sample_offset = (size_t)packet_index * VIBRATION_SAMPLES_PER_PACKET;
            memcpy(slot->samples + sample_offset * 3U, payload + 13, data_bytes);
            slot->packet_bitmap[byte_index] |= bit;
            slot->received_packets++;
            slot->received_samples += samples_in_packet;
            if (((slot->received_packets % 64U) == 0U) ||
                (slot->received_packets == slot->packet_count))
            {
                printf("📦 [振动原始数据] 批次 %u 接收进度：%u/%u 包，%u/%u 点。\n",
                       batch_id, slot->received_packets, slot->packet_count,
                       slot->received_samples,
                       slot->summary_received ? slot->sample_count : 0U);
            }
        }
        slot->sample_rate_hz = sample_rate;
        slot->last_update = time(NULL);
        if ((slot->received_packets == slot->packet_count) ||
            ((slot->received_packets % 32U) == 0U))
        {
            char mac[24] = {0};
            if (lookup_mac_by_short(short_addr, mac))
                publish_vibration_upload_progress(mac, batch_id,
                                                  slot->received_packets,
                                                  slot->packet_count);
        }
        finish_vibration_assembly(slot);
    }
    pthread_mutex_unlock(&g_vibration_mutex);
}

void handle_cmd_8c_vibration_summary(unsigned char *payload, unsigned char len)
{
    /* short(2) + collection(4) + rate(2) + sample_count(4) + 15 * int32 */
    if (len != 72U) return;
    uint16_t short_addr = read_le16(payload);
    uint32_t batch_id = read_le32(payload + 2);
    uint16_t sample_rate = read_le16(payload + 6);
    uint32_t sample_count = read_le32(payload + 8);
    if (sample_count == 0 || sample_count > VIBRATION_MAX_SAMPLE_COUNT) return;
    int32_t features[3][5] = {{0}};
    for (size_t axis = 0; axis < 3; axis++)
    {
        for (size_t feature = 0; feature < 5; feature++)
        {
            features[axis][feature] = (int32_t)read_le32(
                payload + 12U + (axis * 5U + feature) * 4U);
        }
    }

    pthread_mutex_lock(&g_vibration_mutex);
    uint16_t expected_packets = (uint16_t)(
        (sample_count + VIBRATION_SAMPLES_PER_PACKET - 1U) /
        VIBRATION_SAMPLES_PER_PACKET);
    VibrationAssembly_t *slot = find_vibration_assembly(
        short_addr, batch_id, expected_packets);
    if (slot)
    {
        slot->sample_rate_hz = sample_rate;
        slot->sample_count = sample_count;
        memcpy(slot->features, features, sizeof(features));
        slot->summary_received = 1;
        slot->last_update = time(NULL);
        if (slot->received_packets == slot->packet_count)
        {
            finish_vibration_assembly(slot);
            pthread_mutex_unlock(&g_vibration_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&g_vibration_mutex);

    /* 摘要优先落库；后续原始分包完整时更新同一MAC+批次记录。 */
    char mac[24] = {0};
    if (!lookup_mac_by_short(short_addr, mac))
    {
        printf("⚠️ [振动特征] 找不到短地址 0x%04X 对应的节点。\n", short_addr);
        return;
    }
    int match=vibration_summary_matches(mac,batch_id,sample_rate,sample_count,features);
    int summary_replay=match==1;
    int summary_saved = summary_replay || (match==0 && save_vibration_batch(mac, batch_id, sample_rate,
                                             sample_count, NULL, 0U, features) == 0);
    /* The current node sends features only after the waveform (if due) is
     * finalized. Keep this evidence even when the final monitor state is lost. */
    if(summary_saved)mqtt_note_batch(mac,batch_id,MQTT_BATCH_SUMMARY);
    if (summary_saved && !summary_replay)
    {
        update_device_collection_batch_state(mac, "uploading", 0, batch_id);
        publish_collection_data_ready_event(mac, batch_id, 0);
        /* A feature summary alone does not prove a raw stream is due. */
        printf("✅ [振动特征] MAC=%s | 批次=%u | 振动点数=%u | 特征值已入库\n",
               mac, batch_id, sample_count);
    }
    else if (!summary_saved)
    {
        update_device_collection_batch_state(mac, "error", 0, batch_id);
        printf("❌ [振动特征] %s 批次 %u 摘要入库失败。\n", mac, batch_id);
    }
    /* Reuse the existing transparent 0x0D downlink. ACK only after SQL store,
       including duplicate summaries after a lost ACK or gateway restart. */
    uint8_t feature_ack[2U+V2_ACK_LEN];
    v2_w16(feature_ack, short_addr);
    v2_ack(feature_ack+2U, V2_KIND_FEATURES, batch_id, 0U,
           summary_saved ? V2_OK : V2_STORE_FAILED, 0U);
    upload_trace_reply(short_addr,feature_ack+2U);
    if (summary_saved) {
        pthread_mutex_lock(&g_vibration_mutex);
        if(!summary_replay)
            upload_finish(&v2_upload_admission,mac,batch_id,v2_gateway_monotonic_ms(),"振动特征已入库，批次完成释放名额");
        else
            upload_release_reason(&v2_upload_admission,mac,batch_id,v2_gateway_monotonic_ms(),"振动特征重确认，释放名额");
        for (unsigned i=0;i<V2_GATEWAY_SLOTS;i++) {
            V2GatewaySlot *entry=&v2_gateway_slots[i];
            if(entry->batch_id==batch_id && !strcmp(entry->mac,mac)) {
                v2_rx_reset(&entry->rx);memset(entry,0,sizeof(*entry));
            }
        }
        pthread_mutex_unlock(&g_vibration_mutex);
    }
}

void handle_cmd_8d_monitor_state(unsigned char *payload, unsigned char len)
{
    if (len < 8U || payload[2]>1U || payload[3]>MONITOR_STATE_ERROR) return;
    uint16_t short_addr = read_le16(payload);
    int enabled = payload[2] ? 1 : 0;
    const char *state = monitor_state_name(payload[3]);
    uint32_t batch_id = read_le32(payload + 4);
    char mac[24] = {0};
    if (!lookup_mac_by_short(short_addr, mac)) return;
    confirm_visible_node(mac,short_addr,"节点主动上报监测状态");
    if(payload[3]==MONITOR_STATE_CAPTURING || payload[3]==MONITOR_STATE_COMPLETE)
        mqtt_note_batch(mac,batch_id,payload[3]==MONITOR_STATE_CAPTURING?MQTT_BATCH_CAPTURE:MQTT_BATCH_COMPLETE);
    int business_state=payload[3]>=MONITOR_STATE_CAPTURING && payload[3]<=MONITOR_STATE_ERROR;
    uint64_t report_now=v2_gateway_monotonic_ms();
    /* Power/LQI/RSSI may change on a replay; keep link freshness independent
     * from once-per-batch business state, logs and completion timestamps. */
    const unsigned char *telemetry = NULL;
    if (len >= 11U && update_device_link_telemetry(mac, payload[9],
            (int)(int8_t)payload[10], payload[8]) == 0)
        telemetry = payload + 8U;
    if(!device_is_authorized(mac))return;
    if(business_state && monitor_report_replay(&g_report_replays,mac,batch_id,payload[3]))return;
    if(update_device_collection_batch_state(mac,state,payload[3]==MONITOR_STATE_COMPLETE,batch_id)!=0)return;
    if(business_state)report_remember(&g_report_replays,mac,batch_id,GW_RSP_MONITOR_STATE,
                                      payload+3,1,report_now);
    if (payload[3] == MONITOR_STATE_CAPTURING || payload[3] == MONITOR_STATE_UPLOADING ||
        payload[3] == MONITOR_STATE_ERROR || payload[3] == MONITOR_STATE_COMPLETE)
        gateway_log_event(payload[3] == MONITOR_STATE_ERROR ? "ERROR" : "INFO", "node", "collection_state",
            mac, short_addr, batch_id, "%s", payload[3] == MONITOR_STATE_CAPTURING ? "节点开始采集" :
            payload[3] == MONITOR_STATE_UPLOADING ? "采集结束，节点开始上传" :
            payload[3] == MONITOR_STATE_ERROR ? "节点报告采集或上传失败（本报文未提供具体原因）" : "节点报告本轮采集上传完成");
    if (payload[3] == MONITOR_STATE_CAPTURING)
        printf("▶️ [节点采集] MAC=%s | 批次=%u | 开始采集\n", mac, batch_id);
    else if (payload[3] == MONITOR_STATE_UPLOADING)
        printf("⬆️ [数据上传] MAC=%s | 批次=%u | 采集完成，开始上传\n", mac, batch_id);
    else if (payload[3] == MONITOR_STATE_ERROR)
        printf("❌ [节点异常] MAC=%s | 批次=%u | 采集或上传失败\n", mac, batch_id);
    /* Node-reported cached state must not overwrite the user's desired switch. */
    (void)get_device_monitoring(mac, &enabled);
    publish_monitor_event(mac, enabled, state, batch_id, telemetry);
}

void handle_cmd_9a_network_restore(unsigned char *payload, unsigned char len)
{
    if (len < 2U)
    {
        printf("⚠️ [Zigbee网络恢复] 0x9A报文长度不足：%u\n", len);
        return;
    }
    unsigned char result = payload[0];
    unsigned char status = payload[1];
    pthread_mutex_lock(&g_network_info_mutex);
    g_network_restore_result = result;
    g_network_restore_status = status;
    pthread_mutex_unlock(&g_network_info_mutex);
    if (result == GW_RESTORE_SUCCESS && len >= 5U)
    {
        uint16_t pan_id = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
        unsigned char channel = payload[4];
        printf("✅ [Zigbee网络恢复] 已恢复原网络 | PAN ID: 0x%04X | 实际信道: %u | 状态: 0x%02X\n",
               pan_id, channel, status);
        if (channel >= 11U && channel <= 26U)
            (void)save_zigbee_network_state(pan_id, channel, "restored");
    }
    else if (result == GW_RESTORE_NO_DATA)
    {
        printf("ℹ️ [Zigbee网络恢复] 未发现持久化网络，协调器将自动扫描11～26信道并建网。\n");
    }
    else
    {
        printf("⚠️ [Zigbee网络恢复] 旧网络恢复失败，状态=0x%02X；协调器将自动扫描11～26信道并重新建网。\n",
               status);
    }
}

static int send_monitor_resume_schedule(const char *mac,uint16_t address,const MonitorConfig_t *cfg,
                                        int enabled)
{
    unsigned char payload[2U+CONTROL_RESUME_SCHEDULE_SIZE]={0};
    uint32_t deadline=0,delay=0;
    if (enabled && get_sampling_resume_plan(mac,cfg->auto_interval_sec,
        (long long)time(NULL)*1000,&deadline,&delay)!=1) {
        gateway_log_event("ERROR","backend","sampling_resume_failed",mac,address,-1,
                          "无法计算恢复后的采集时间，节点继续等待");
        return 0;
    }
    payload[0]=(uint8_t)address;payload[1]=(uint8_t)(address>>8);
    payload[2]=(uint8_t)enabled;
    write_le32(payload+3,deadline);write_le32(payload+7,delay);
    pthread_mutex_lock(&g_uart_mutex);
    int available=g_uart_fd>=0;
    if (available)
        send_hex_frame_to_uart(g_uart_fd,CONTROL_RESUME_SCHEDULE_CMD,sizeof(payload),payload);
    pthread_mutex_unlock(&g_uart_mutex);
    if (available && enabled) {
        char stamp[32];time_t planned=(time_t)deadline;struct tm local={0};
#ifdef _WIN32
        (void)localtime_s(&local,&planned);
#else
        (void)localtime_r(&planned,&local);
#endif
        (void)strftime(stamp,sizeof(stamp),"%Y-%m-%d %H:%M:%S",&local);
        printf("[采集时间恢复] MAC=%s | 下次采集=%s | 距下次采集=%u毫秒 | 旧批次可先申请上传\n",mac,stamp,delay);
        gateway_log_event("INFO","backend","sampling_resume",mac,address,-1,
                          "下次采集=%s，等待%u毫秒；旧批次可先申请上传",stamp,delay);
    }
    return available;
}


static void handle_control_status(const unsigned char *payload, unsigned len)
{
    int final=len==CONTROL_FINAL_SIZE+4U && control_final_status_valid(payload+2,CONTROL_FINAL_SIZE);
    if (!final && (len!=CONTROL_STATUS_SIZE+4U || !control_status_valid(payload+2,CONTROL_STATUS_SIZE))) return;
    uint16_t address=read_le16(payload);
    char mac[32], short_text[8];
    if (!lookup_mac_by_short(address,mac)) return;
    confirm_visible_node(mac,address,"节点主动上报有效控制状态");
    snprintf(short_text,sizeof(short_text),"%04X",address);
    update_device_online(mac,short_text);
    (void)update_device_link_telemetry(mac,payload[len-2U],(int8_t)payload[len-1U],payload[8]);
    const unsigned char *status=payload+2;
    if (final && (status[1]&CONTROL_SAVE_FAILED)) {
        printf("[节点配置保存失败] MAC=%s | revision=%u\n",mac,control_read_hash(status+8));
        return;
    }
    if (device_admission_requested(mac)) {
        const unsigned int applied=CONTROL_VALID|CONTROL_ENABLED|CONTROL_APPLIED;
        if ((status[1]&applied)==applied && admission_delivery_apply(mac)>0) {
            printf("[节点授权确认] MAC=%s | 节点已应用授权，按本地采样配置运行\n",mac);
            gateway_log_event("INFO","node","node_authorized",mac,address,-1,
                "节点已应用授权，按本地采样配置运行");
        }
        return;
    }
    if (!device_is_authorized(mac)) return;
    MonitorConfig_t cfg=load_monitor_config_for_mac(mac);
    unsigned char desired[31]; char resolved[32];
    if (!build_monitor_config_payload(address,&cfg,desired,resolved)) return;
    uint32_t hash=control_fingerprint(desired+2,29);
    int enabled=(desired[28]&0x02U)!=0;
    if (!final && (status[1]&CONTROL_NEEDS_SCHEDULE)) {
        (void)send_monitor_resume_schedule(mac,address,&cfg,enabled);
        return;
    }
    if ((status[1]&CONTROL_VALID) && control_read_hash(status+2)==hash &&
        !!(status[1]&CONTROL_ENABLED)==enabled) {
        if (control_delivery(mac,hash,2)) {
            (void)update_device_expected_interval(mac,cfg.auto_interval_sec);
            printf("[节点配置确认] MAC=%s | 已应用配置=%08X | 在线监测=%s\n",
                   mac,hash,enabled?"开启":"关闭");
            gateway_log_event("INFO","node","monitor_applied",mac,address,-1,
                              "节点已应用配置=%08X，在线监测=%s",hash,enabled?"开启":"关闭");
        }
        return; /* Normal heartbeats never solicit a full configuration reply. */
    }
    if (final) return;
    if (!(status[1]&CONTROL_UPLOAD_CONFIG_WINDOW)) {
        if (!!(status[1]&CONTROL_ENABLED)!=enabled)
            (void)send_monitor_control(address,MONITOR_OP_SET_ENABLED,enabled);
        return;
    }
    if (control_delivery(mac,hash,0)) {
        config_diag_print(mg_millis(),"WINDOW | MAC=%s | 上传已结束，当前配置与保存配置不一致，开始同步",mac);
        (void)send_monitor_config_scoped(address,&cfg);
    }
}

void handle_cmd_99_config_query(unsigned char *payload, unsigned char len)
{
    if (len < 3U || payload[2]<1U || payload[2]>3U) return;
    uint16_t short_addr=read_le16(payload);
    char mac[32],short_text[8];
    if (!lookup_mac_by_short(short_addr,mac)) return;
    confirm_visible_node(mac,short_addr,"节点入网后主动查询配置");
    snprintf(short_text,sizeof(short_text),"%04X",short_addr);
    update_device_online(mac,short_text);
    config_diag_print(mg_millis(),"DROP99 | MAC=%s | 旧版查询未声明上传已结束，跳过采样配置下发",mac);
}


//解析器（状态机）：用来拆解协调器发来的报文
void uart_parse_byte(UartParser_t *parser, unsigned char c) 
{
    switch (parser->state) 
    {
        case STATE_SOF:
            if (c == 0xFE) parser->state = STATE_CMD;
            break;
            
        case STATE_CMD:
            parser->rx_cmd = c;
            parser->bcc_calc = c;
            parser->state = STATE_LEN;
            break;

        case STATE_LEN:
            parser->payload_len = c;
            parser->bcc_calc ^= c;
            parser->payload_idx = 0;
            if (parser->payload_len > 0) parser->state = STATE_DATA;
            else parser->state = STATE_BCC;
            break;
            
        case STATE_DATA:
            parser->rx_payload[parser->payload_idx++] = c;
            parser->bcc_calc ^= c;
            if (parser->payload_idx == parser->payload_len) parser->state = STATE_BCC;
            break;
            
        case STATE_BCC:
            if (parser->bcc_calc == c) 
            {
                // 校验通过，扔给分发器执行业务
                dispatch_uart_command(parser->rx_cmd, parser->rx_payload, parser->payload_len);
            } 
            else 
            {
                printf("⚠️ [解析器] BCC 校验失败 (算得 %02X, 收到 %02X)！\n", parser->bcc_calc, c);
            }
            // 复位状态机，准备接下一帧
            parser->state = STATE_SOF; 
            break;
    }
}

/*********************************************************************
 * 独立任务 1：WebSocket 广播投递员
 *********************************************************************/
void task_broadcast_ws(struct mg_connection *c) 
{
    //互斥锁  保证将这个新设备推送给所有的前端设备后，才能更改g_ws_msg_buffer中的内容
    pthread_mutex_lock(&g_ws_mutex);
    if (g_has_new_ws_msg == 1)
    {
        const char *event;
        while((event=ws_event_front(&g_ws_events))!=NULL) {
            for (struct mg_connection *t=c->mgr->conns;t;t=t->next)
                if(t->is_websocket) {
                    if(t->send.len>1024U*1024U)t->is_closing=1;
                    else mg_ws_send(t,event,strlen(event),WEBSOCKET_OP_TEXT);
                }
            ws_event_pop(&g_ws_events);
        }
        if(g_ws_events.overflow){fprintf(stderr,"[WebSocket] 通知队列溢出 %u 次，页面将通过定时补查恢复\n",g_ws_events.overflow);g_ws_events.overflow=0;}
        g_has_new_ws_msg = 0; 
    }
    pthread_mutex_unlock(&g_ws_mutex);
}

/*********************************************************************
 * 独立任务 2：流量整形红绿灯 (盘问调度)
 *********************************************************************/
void task_dispatch_queries(void) 
{
    //相比于 BLE，Zigbee 的带宽极低（只有 250kbps），对“空中碰撞”极其敏感。
    //如果十几台设备同时入网，网关不加节制地瞬间连发十几条 0x04 询问指令，
    //CC1352R 协调器大概率会直接死机或者出现严重的丢包。

    static unsigned long last_dispatch_time = 0;//创建静态变量，只有这个函数能访问
    unsigned long current_time = (unsigned long)mg_millis(); 

    if (current_time - last_dispatch_time >= 300) 
    {
        uint16_t target_short_addr;
        if (dequeue_device(&target_short_addr)) //提取询问目标信息设备的短地址
        {
            // 提取短地址的低位和高位作为 Payload
            unsigned char payload[2];
            payload[0] = (unsigned char)(target_short_addr & 0xFF);
            payload[1] = (unsigned char)((target_short_addr >> 8) & 0xFF);
            //串口发送
            send_hex_frame_to_uart(g_uart_fd, 0x04, 2, payload);
        }
        last_dispatch_time = current_time; 
    }
}

/*********************************************************************
 * API 业务 1：处理添加白名单
 *********************************************************************/
void api_handle_device_add(struct mg_connection *c, struct mg_http_message *hm)
{
    if(mg_strcmp(hm->method,mg_str("POST"))!=0) {mg_http_reply(c,405,join_headers,"{}");return;}
    cJSON *root=cJSON_ParseWithLength(hm->body.buf,hm->body.len);
    cJSON *mac=cJSON_GetObjectItem(root,"mac");unsigned char bytes[8];
    if(!cJSON_IsString(mac) || mac_string_to_bytes(mac->valuestring,bytes)!=0) {
        cJSON_Delete(root);mg_http_reply(c,400,join_headers,"{\"status\":\"error\",\"msg\":\"MAC 格式无效\"}");return;
    }
    char normalized[24];
    snprintf(normalized,sizeof(normalized),"%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
        bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7]);
    int exists=is_device_in_db(normalized);
    int rc=insert_new_device(normalized,"Unknown Device");
    if(rc==0) (void)clear_revoked_node(normalized);
    cJSON_Delete(root);
    /* Manual registration clears a removal block, but never opens the radio. */
    mg_http_reply(c,rc?500:200,join_headers,"{\"status\":\"%s\"}",rc?"error":exists?"exists":"success");
}

static int find_serial_device(SerialDevice_t *devices, int count, const char *path)
{
    for (int i = 0; i < count; i++)
    {
        if (strcmp(devices[i].path, path) == 0) return i;
    }
    return -1;
}

/* 迁移期间每秒查询一次协调器。超时仍未看到目标信道时，以最后一次
 * 查询到的实际信道结束操作并报告失败。 */
void task_channel_migration(void)
{
    channel_migration_commit_if_ready();
    unsigned long now = (unsigned long)mg_millis();
    int should_query = 0;
    int timed_out = 0;
    unsigned char current_channel = 0U;

    pthread_mutex_lock(&g_channel_operation_mutex);
    if (g_channel_operation.phase == CHANNEL_OPERATION_PENDING)
    {
        if (g_channel_operation.deadline_ms != 0U &&
            (long)(now - g_channel_operation.deadline_ms) >= 0)
        {
            timed_out = 1;
            current_channel = g_channel_operation.current_channel;
        }
        else if (g_channel_operation.next_query_ms != 0U &&
                 (long)(now - g_channel_operation.next_query_ms) >= 0)
        {
            should_query = 1;
            g_channel_operation.next_query_ms = now + 1000U;
        }
    }
    pthread_mutex_unlock(&g_channel_operation_mutex);

    if (timed_out)
    {
        if (current_channel < 11U || current_channel > 26U)
        {
            pthread_mutex_lock(&g_network_info_mutex);
            current_channel = g_network_channel;
            pthread_mutex_unlock(&g_network_info_mutex);
        }
        finish_channel_operation(CHANNEL_OPERATION_FAILED, current_channel,
                                 0xFFU, "等待实际信道切换超时");
        printf("❌ [Zigbee信道配置] 等待切换超时，当前实际信道=%u。\n",
               current_channel);
    }
    else if (should_query)
    {
        request_network_info();
    }
}

static const char *serial_device_kind(const char *path)
{
    if (strncmp(path, "/dev/ttyACM", 11) == 0 || strncmp(path, "/dev/ttyUSB", 11) == 0)
    {
        return "usb";
    }
    return "native";
}

static void describe_serial_device(const char *path, const char *by_id_name, char *description, size_t size)
{
    if (by_id_name && strstr(by_id_name, "XDS110"))
    {
        if (strstr(by_id_name, "-if00"))
        {
            snprintf(description, size, "XDS110 Application/User UART");
        }
        else if (strstr(by_id_name, "-if03"))
        {
            snprintf(description, size, "XDS110 Auxiliary Data Port");
        }
        else
        {
            snprintf(description, size, "XDS110 串口");
        }
    }
    else if (by_id_name && (strstr(by_id_name, "CH340") || strstr(by_id_name, "ch340")))
    {
        snprintf(description, size, "USB-SERIAL CH340");
    }
    else if (strncmp(path, "/dev/ttyUSB", 11) == 0)
    {
        snprintf(description, size, "USB 转串口");
    }
    else if (strncmp(path, "/dev/ttyACM", 11) == 0)
    {
        snprintf(description, size, "USB CDC/ACM 串口");
    }
    else
    {
        snprintf(description, size, "板载 UART");
    }
}

static int is_serial_device_present(const char *path)
{
    struct stat device_stat;
    if (stat(path, &device_stat) != 0 || !S_ISCHR(device_stat.st_mode)) return 0;

    // PC Linux常预建ttyS0～ttyS31占位节点；RK3506实际UART会对应独立的平台设备。
    if (strncmp(path, "/dev/ttyS", 9) == 0)
    {
        const char *tty_name = path + strlen("/dev/");
        char sysfs_path[PATH_MAX] = {0};
        char resolved_device[PATH_MAX] = {0};
        snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/tty/%s/device", tty_name);
        if (!realpath(sysfs_path, resolved_device)) return 0;
        if (strcmp(resolved_device, "/sys/devices/platform/serial8250") == 0) return 0;
    }
    return 1;
}

static int add_serial_device(SerialDevice_t *devices, int count, int max_devices,
                             const char *path, const char *stable_path, const char *by_id_name)
{
    if (!path || path[0] == '\0' || count >= max_devices) return count;
    if (!is_serial_device_present(path)) return count;

    int index = find_serial_device(devices, count, path);
    if (index < 0)
    {
        index = count++;
        snprintf(devices[index].path, sizeof(devices[index].path), "%s", path);
        snprintf(devices[index].kind, sizeof(devices[index].kind), "%s", serial_device_kind(path));
        devices[index].accessible = access(path, R_OK | W_OK) == 0;
        describe_serial_device(path, by_id_name, devices[index].description,
                               sizeof(devices[index].description));
    }

    if (stable_path && stable_path[0] != '\0')
    {
        snprintf(devices[index].stable_path, sizeof(devices[index].stable_path), "%s", stable_path);
        describe_serial_device(path, by_id_name, devices[index].description,
                               sizeof(devices[index].description));
    }
    return count;
}

static int add_serial_pattern(SerialDevice_t *devices, int count, int max_devices, const char *pattern)
{
    glob_t matches = {0};
    if (glob(pattern, 0, NULL, &matches) == 0)
    {
        for (size_t i = 0; i < matches.gl_pathc; i++)
        {
            count = add_serial_device(devices, count, max_devices, matches.gl_pathv[i], NULL, NULL);
        }
    }
    globfree(&matches);
    return count;
}

// 只枚举当前存在的Linux串口，不打开设备，也不判断哪个串口连接协调器
static int enumerate_serial_devices(SerialDevice_t *devices, int max_devices)
{
    int count = 0;
    glob_t by_id_matches = {0};

    // USB串口优先读取稳定的by-id名称，用于区分XDS110、CH340等设备
    if (glob("/dev/serial/by-id/*", 0, NULL, &by_id_matches) == 0)
    {
        for (size_t i = 0; i < by_id_matches.gl_pathc; i++)
        {
            char resolved_path[PATH_MAX] = {0};
            if (!realpath(by_id_matches.gl_pathv[i], resolved_path)) continue;

            const char *name = strrchr(by_id_matches.gl_pathv[i], '/');
            name = name ? name + 1 : by_id_matches.gl_pathv[i];
            count = add_serial_device(devices, count, max_devices, resolved_path,
                                      by_id_matches.gl_pathv[i], name);
        }
    }
    globfree(&by_id_matches);

    // 兼容没有by-id链接的USB串口，以及RK3506等平台的板载UART
    const char *patterns[] = {
        "/dev/ttyACM*", "/dev/ttyUSB*", "/dev/ttyS*",
        "/dev/ttyAMA*", "/dev/ttyTHS*", "/dev/ttyFIQ*"
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++)
    {
        count = add_serial_pattern(devices, count, max_devices, patterns[i]);
    }
    return count;
}

// 返回Linux当前识别到的串口列表，供前端下拉框选择
void api_handle_serial_devices_get(struct mg_connection *c)
{
    SerialDevice_t devices[MAX_SERIAL_DEVICES] = {0};
    int count = enumerate_serial_devices(devices, MAX_SERIAL_DEVICES);

    cJSON *response = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(response, "devices");
    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "path", devices[i].path);
        cJSON_AddStringToObject(item, "description", devices[i].description);
        cJSON_AddStringToObject(item, "kind", devices[i].kind);
        cJSON_AddBoolToObject(item, "accessible", devices[i].accessible);
        if (devices[i].stable_path[0] != '\0')
        {
            cJSON_AddStringToObject(item, "stable_path", devices[i].stable_path);
        }
        else
        {
            cJSON_AddNullToObject(item, "stable_path");
        }
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddNumberToObject(response, "count", count);

    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    mg_http_reply(c, 200,
                  "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  json ? json : "{\"devices\":[],\"count\":0}");
    if (json) free(json);
}

// 返回Zigbee网络实时信息和数据库中保存的信道策略
void api_handle_system_settings_get(struct mg_connection *c)
{
    request_network_info();

    pthread_mutex_lock(&g_network_info_mutex);
    uint16_t pan_id = g_network_pan_id;
    unsigned char channel = g_network_channel;
    unsigned char device_state = g_network_device_state;
    unsigned char startup_source = g_network_startup_source;
    unsigned char network_state = g_network_state;
    uint64_t last_seen_monotonic_ms = g_network_last_seen_monotonic_ms;
    uint64_t last_seen_epoch_ms = g_network_last_seen_epoch_ms;
    int has_network_info = g_has_network_info;
    pthread_mutex_unlock(&g_network_info_mutex);

    uint64_t now_ms = coordinator_clock_ms(CLOCK_MONOTONIC);
    int uart_available;
    pthread_mutex_lock(&g_uart_mutex);
    uart_available = g_uart_fd >= 0;
    pthread_mutex_unlock(&g_uart_mutex);
    int coordinator_online = uart_available && has_network_info &&
        last_seen_monotonic_ms != 0U && now_ms >= last_seen_monotonic_ms &&
        now_ms - last_seen_monotonic_ms <= COORDINATOR_RESPONSE_TIMEOUT_MS;

    char channel_strategy[16] = "auto";
    int target_channel = 0;
    (void)load_zigbee_channel_config(channel_strategy, sizeof(channel_strategy),
                                     &target_channel);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "channel_strategy", channel_strategy);
    cJSON_AddBoolToObject(response, "coordinator_online", coordinator_online);
    cJSON_AddBoolToObject(response, "serial_available", uart_available);
    if (last_seen_epoch_ms != 0U)
        cJSON_AddNumberToObject(response, "coordinator_last_seen_ms",
                                (double)last_seen_epoch_ms);
    else
        cJSON_AddNullToObject(response, "coordinator_last_seen_ms");
    cJSON_AddStringToObject(response, "network_state",
                            has_network_info ? network_state_text(network_state)
                                             : "unknown");
    cJSON_AddNumberToObject(response, "network_state_code", network_state);
    if (strcmp(channel_strategy, "manual") == 0)
        cJSON_AddNumberToObject(response, "target_channel", target_channel);
    else cJSON_AddNullToObject(response, "target_channel");
    if (has_network_info && network_state == GW_NETWORK_STATE_READY &&
        channel >= 11 && channel <= 26)
    {
        cJSON_AddNumberToObject(response, "pan_id", pan_id);
        cJSON_AddNumberToObject(response, "current_channel", channel);
        cJSON_AddNumberToObject(response, "device_state", device_state);
        cJSON_AddStringToObject(response, "network_source",
            startup_source == GW_NETWORK_SOURCE_RESTORED ? "restored" :
            (startup_source == GW_NETWORK_SOURCE_AUTO_FORM ? "auto_scan" : "realtime"));
    }
    else
    {
        cJSON_AddNullToObject(response, "pan_id");
        cJSON_AddNullToObject(response, "current_channel");
        cJSON_AddNullToObject(response, "device_state");
        cJSON_AddStringToObject(response, "network_source", "unknown");
    }

    pthread_mutex_lock(&g_channel_operation_mutex);
    ChannelOperation_t channel_operation = g_channel_operation;
    pthread_mutex_unlock(&g_channel_operation_mutex);
    cJSON *operation = cJSON_AddObjectToObject(response, "channel_operation");
    cJSON_AddStringToObject(operation, "state",
                            channel_operation_phase_text(channel_operation.phase));
    cJSON_AddStringToObject(operation, "strategy",
                            channel_operation.strategy == 1U ? "manual" : "auto");
    if (channel_operation.target_channel >= 11U &&
        channel_operation.target_channel <= 26U)
        cJSON_AddNumberToObject(operation, "target_channel",
                                channel_operation.target_channel);
    else cJSON_AddNullToObject(operation, "target_channel");
    if (channel_operation.current_channel >= 11U &&
        channel_operation.current_channel <= 26U)
        cJSON_AddNumberToObject(operation, "current_channel",
                                channel_operation.current_channel);
    else cJSON_AddNullToObject(operation, "current_channel");
    cJSON_AddNumberToObject(operation, "coordinator_status",
                            channel_operation.coordinator_status);
    cJSON_AddNumberToObject(operation, "prepared_nodes",
                            channel_operation.prepared_count);
    cJSON_AddNumberToObject(operation, "total_nodes",
                            channel_operation.node_count);
    cJSON_AddStringToObject(operation, "message", channel_operation.message);

    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    mg_http_reply(c, 200,
                  "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  json ? json : "{}");
    if (json) free(json);
}

void api_handle_channel_settings(struct mg_connection *c, struct mg_http_message *hm)
{
    cJSON *root = parse_http_json(hm);
    cJSON *strategy_item = root ? cJSON_GetObjectItem(root, "channel_strategy") : NULL;
    cJSON *target_item = root ? cJSON_GetObjectItem(root, "target_channel") : NULL;
    const char *strategy = cJSON_IsString(strategy_item) ? strategy_item->valuestring : NULL;
    int is_auto = strategy && strcmp(strategy, "auto") == 0;
    int is_manual = strategy && strcmp(strategy, "manual") == 0;
    int target = cJSON_IsNumber(target_item) ? target_item->valueint : 0;
    if ((!is_auto && !is_manual) || (is_manual && (target < 11 || target > 26)))
    {
        cJSON_Delete(root);
        mg_http_reply(c, 400, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"信道策略或信道编号无效\"}");
        return;
    }

    pthread_mutex_lock(&g_channel_operation_mutex);
    int operation_pending =
        g_channel_operation.phase == CHANNEL_OPERATION_PENDING;
    pthread_mutex_unlock(&g_channel_operation_mutex);
    if (operation_pending)
    {
        cJSON_Delete(root);
        mg_http_reply(c, 409,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"已有信道操作等待执行，请勿重复提交\"}");
        return;
    }

    DeviceRecord_t *records = NULL;
    int count = get_all_devices(&records);
    if (count < 0)
    {
        cJSON_Delete(root);
        mg_http_reply(c, 500, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"读取节点列表失败\"}");
        return;
    }
    unsigned char payload[204] = {0};
    DeviceRecord_t migration_records[CHANNEL_MIGRATION_MAX_NODES];
    unsigned char address_count = 0U;
    for (int i = 0; i < count; ++i)
    {
        if (!records[i].join_confirmed) continue;
        unsigned int short_addr = 0U;
        if (address_count < 100U && sscanf(records[i].short_addr, "%x", &short_addr) == 1)
        {
            write_le16(payload + 4U + (size_t)address_count * 2U,
                       (uint16_t)short_addr);
            migration_records[address_count] = records[i];
            address_count++;
        }
    }

    /* 手动策略只有在 0x8A 确认实际信道后才持久化；否则重启页面会把
     * 一个尚未成功的目标信道误显示成当前策略。自动策略不迁移现网，
     * 可以立即保存。 */
    if (is_auto && save_zigbee_channel_config("auto", 0) != 0)
    {
        free(records);
        cJSON_Delete(root);
        mg_http_reply(c, 500, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"信道策略保存失败\"}");
        return;
    }

    payload[0] = (unsigned char)(is_auto ? 0U : 1U);
    payload[1] = (unsigned char)target;
    payload[2] = 0U;
    payload[3] = 1U;
    unsigned char payload_len = 4U;
    int wait_for_nodes = is_manual && address_count > 0U;
    pthread_mutex_lock(&g_uart_mutex);
    int available = g_uart_fd >= 0;
    if (available)
    {
        begin_channel_operation((unsigned char)(is_auto ? 0U : 1U),
                                (unsigned char)target, 2U,
                                migration_records,
                                wait_for_nodes ? address_count : 0U,
                                wait_for_nodes);
        if (!wait_for_nodes)
            send_hex_frame_to_uart(g_uart_fd, GW_CMD_CHANNEL_CONFIG,
                                   payload_len, payload);
    }
    pthread_mutex_unlock(&g_uart_mutex);
    free(records);
    cJSON_Delete(root);

    if (!available)
    {
        mg_http_reply(c, 503, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"协调器串口未连接；策略已保存但尚未应用\"}");
        return;
    }
    printf("[Zigbee信道配置] 已提交策略=%s, 目标信道=%d, 等待节点=%u。\n",
           is_auto ? "自动" : "手动", target, address_count);
    mg_http_reply(c, 202, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "{\"status\":\"accepted\",\"migration\":%s,\"device_count\":%d,"
                  "\"migration_delay_sec\":%u,"
                  "\"msg\":\"%s\"}",
                  wait_for_nodes ? "true" : "false", address_count, 0U,
                  is_auto ? "自动信道策略已保存；当前网络保持现有信道，重新建网时自动选择"
                          : (wait_for_nodes
                              ? "信道迁移已保存，等待各节点在上传完成后的接收窗口或下次唤醒窗口接收通知"
                              : "协调器信道切换请求已提交"));
}


// 无RTC网关由浏览器提供当前Unix时间；仅接受合理年份范围，避免错误时间污染历史数据。
void api_handle_system_time_sync(struct mg_connection *c, struct mg_http_message *hm)
{
    if (hm->body.len == 0 || hm->body.len > 256)
    {
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Invalid request body\"}");
        return;
    }

    char body[257];
    memcpy(body, hm->body.buf, hm->body.len);
    body[hm->body.len] = '\0';

    cJSON *root = cJSON_Parse(body);
    cJSON *client_time_ms = root ? cJSON_GetObjectItem(root, "client_time_ms") : NULL;

    if (!cJSON_IsNumber(client_time_ms))
    {
        if (root) cJSON_Delete(root);
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"client_time_ms is required\"}");
        return;
    }

    double requested_ms = client_time_ms->valuedouble;
    cJSON_Delete(root);

    if (!isfinite(requested_ms) ||
        requested_ms < (double)MIN_VALID_SYSTEM_EPOCH * 1000.0 ||
        requested_ms >= (double)MAX_VALID_SYSTEM_EPOCH * 1000.0)
    {
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"client time is outside the accepted range\"}");
        return;
    }

    long long requested_total_ms = (long long)requested_ms;
    long long requested_sec = requested_total_ms / 1000;
    long long requested_ms_remainder = requested_total_ms % 1000;

    time_t current_sec = time(NULL);
    long long drift_sec = requested_sec - (long long)current_sec;
    int corrected = current_sec < MIN_VALID_SYSTEM_EPOCH ||
                    current_sec >= MAX_VALID_SYSTEM_EPOCH ||
                    llabs(drift_sec) > 2;

    if (corrected)
    {
        struct timespec requested_time = {
            .tv_sec = (time_t)requested_sec,
            .tv_nsec = (long)requested_ms_remainder * 1000000L
        };

        if (clock_settime(CLOCK_REALTIME, &requested_time) != 0)
        {
            char response[256];
            snprintf(response, sizeof(response),
                     "{\"status\":\"error\",\"msg\":\"clock_settime failed: %s\"}",
                     strerror(errno));
            mg_http_reply(c, 500,
                          "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                          response);
            return;
        }

        refresh_online_device_activity();
        int deleted_count = cleanup_sensor_history(SENSOR_HISTORY_RETENTION_SEC);
        printf("🕒 [系统授时] 已采用浏览器时间，清理 %d 条无效或过期历史。\n",
               deleted_count > 0 ? deleted_count : 0);
    }

    struct timespec actual_time = {0};
    clock_gettime(CLOCK_REALTIME, &actual_time);
    long long actual_ms = (long long)actual_time.tv_sec * 1000LL +
                          actual_time.tv_nsec / 1000000L;
    char response[192];
    snprintf(response, sizeof(response),
             "{\"status\":\"success\",\"corrected\":%s,\"server_time_ms\":%lld}",
             corrected ? "true" : "false", actual_ms);
    mg_http_reply(c, 200,
                  "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  response);
}


static void json_add_number_or_null(cJSON *root, const char *name,
                                    int valid, double value)
{
    if (valid)
    {
        cJSON_AddNumberToObject(root, name, value);
    }
    else
    {
        cJSON_AddNullToObject(root, name);
    }
}


/* 读取当前运行后端的Linux主机资源。所有数值都来自/proc、statvfs和
 * 实际数据库文件；读取失败的单项返回null，绝不使用默认或演示数据。 */
void api_handle_system_resources_get(struct mg_connection *c)
{
    static unsigned long long previous_cpu_total = 0;
    static unsigned long long previous_cpu_idle = 0;

    int cpu_valid = 0;
    double cpu_usage_percent = 0.0;
    FILE *cpu_file = fopen("/proc/stat", "r");
    if (cpu_file)
    {
        char line[512] = {0};
        if (fgets(line, sizeof(line), cpu_file))
        {
            unsigned long long user = 0, nice = 0, system = 0, idle = 0;
            unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
            int fields = sscanf(line,
                                "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                                &user, &nice, &system, &idle, &iowait,
                                &irq, &softirq, &steal);
            if (fields >= 4)
            {
                unsigned long long idle_total = idle + iowait;
                unsigned long long total = user + nice + system + idle +
                                           iowait + irq + softirq + steal;
                if (previous_cpu_total > 0 && total > previous_cpu_total)
                {
                    unsigned long long total_delta = total - previous_cpu_total;
                    unsigned long long idle_delta = idle_total >= previous_cpu_idle
                        ? idle_total - previous_cpu_idle
                        : 0;
                    cpu_usage_percent = 100.0 *
                        (double)(total_delta - (idle_delta > total_delta
                            ? total_delta
                            : idle_delta)) /
                        (double)total_delta;
                    cpu_valid = 1;
                }
                previous_cpu_total = total;
                previous_cpu_idle = idle_total;
            }
        }
        fclose(cpu_file);
    }

    int memory_valid = 0;
    unsigned long long memory_total_bytes = 0;
    unsigned long long memory_used_bytes = 0;
    FILE *memory_file = fopen("/proc/meminfo", "r");
    if (memory_file)
    {
        char line[256];
        unsigned long long total_kb = 0;
        unsigned long long available_kb = 0;
        while (fgets(line, sizeof(line), memory_file))
        {
            if (sscanf(line, "MemTotal: %llu kB", &total_kb) == 1)
            {
                continue;
            }
            if (sscanf(line, "MemAvailable: %llu kB", &available_kb) == 1)
            {
                continue;
            }
        }
        fclose(memory_file);
        if (total_kb > 0 && available_kb <= total_kb)
        {
            memory_total_bytes = total_kb * 1024ULL;
            memory_used_bytes = (total_kb - available_kb) * 1024ULL;
            memory_valid = 1;
        }
    }

    int disk_valid = 0;
    unsigned long long disk_total_bytes = 0;
    unsigned long long disk_used_bytes = 0;
    struct statvfs disk_info;
    /* 后端可能从/mnt/hgfs等共享目录启动，必须读取Linux根文件系统，
     * 否则虚拟机阶段会误报Windows宿主机共享盘容量。 */
    if (statvfs("/", &disk_info) == 0 && disk_info.f_blocks > 0)
    {
        disk_total_bytes = (unsigned long long)disk_info.f_blocks *
                           (unsigned long long)disk_info.f_frsize;
        unsigned long long available_bytes =
            (unsigned long long)disk_info.f_bavail *
            (unsigned long long)disk_info.f_frsize;
        disk_used_bytes = disk_total_bytes >= available_bytes
            ? disk_total_bytes - available_bytes
            : 0;
        disk_valid = 1;
    }

    int uptime_valid = 0;
    double uptime_seconds = 0.0;
    FILE *uptime_file = fopen("/proc/uptime", "r");
    if (uptime_file)
    {
        uptime_valid = fscanf(uptime_file, "%lf", &uptime_seconds) == 1 &&
                       uptime_seconds >= 0.0;
        fclose(uptime_file);
    }

    int database_valid = 0;
    unsigned long long database_size_bytes = 0;
    struct stat database_info;
    if (stat("data/gateway.db", &database_info) == 0 &&
        database_info.st_size >= 0)
    {
        database_size_bytes = (unsigned long long)database_info.st_size;
        database_valid = 1;
    }

    struct timespec sampled_time = {0};
    clock_gettime(CLOCK_REALTIME, &sampled_time);
    long long sampled_at_ms = (long long)sampled_time.tv_sec * 1000LL +
                              sampled_time.tv_nsec / 1000000L;

    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        mg_http_reply(c, 500,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Out of memory\"}");
        return;
    }

    cJSON_AddStringToObject(root, "status", "success");
    json_add_number_or_null(root, "cpu_usage_percent", cpu_valid,
                            cpu_usage_percent);
    json_add_number_or_null(root, "memory_total_bytes", memory_valid,
                            (double)memory_total_bytes);
    json_add_number_or_null(root, "memory_used_bytes", memory_valid,
                            (double)memory_used_bytes);
    json_add_number_or_null(root, "memory_usage_percent", memory_valid,
                            memory_valid
                                ? 100.0 * (double)memory_used_bytes /
                                  (double)memory_total_bytes
                                : 0.0);
    json_add_number_or_null(root, "disk_total_bytes", disk_valid,
                            (double)disk_total_bytes);
    json_add_number_or_null(root, "disk_used_bytes", disk_valid,
                            (double)disk_used_bytes);
    json_add_number_or_null(root, "disk_usage_percent", disk_valid,
                            disk_valid
                                ? 100.0 * (double)disk_used_bytes /
                                  (double)disk_total_bytes
                                : 0.0);
    json_add_number_or_null(root, "uptime_seconds", uptime_valid,
                            uptime_seconds);
    json_add_number_or_null(root, "database_size_bytes", database_valid,
                            (double)database_size_bytes);
    cJSON_AddNumberToObject(root, "sampled_at_ms", (double)sampled_at_ms);

    char *response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!response)
    {
        mg_http_reply(c, 500,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Out of memory\"}");
        return;
    }

    mg_http_reply(c, 200,
                  "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "%s", response);
    free(response);
}


// 测试或保存串口路径；保存前必须先成功打开新串口，避免丢失当前连接
void api_handle_serial_settings(struct mg_connection *c, struct mg_http_message *hm, int save_config)
{
    char *body = malloc(hm->body.len + 1);
    if (!body)
    {
        mg_http_reply(c, 500, "Access-Control-Allow-Origin: *\r\n", "{\"status\":\"error\",\"msg\":\"Out of memory\"}");
        return;
    }
    memcpy(body, hm->body.buf, hm->body.len);
    body[hm->body.len] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *serial_path = root ? cJSON_GetObjectItem(root, "serial_path") : NULL;
    if (!cJSON_IsString(serial_path) || serial_path->valuestring[0] == '\0' ||
        strlen(serial_path->valuestring) >= sizeof(g_serial_path) ||
        strncmp(serial_path->valuestring, "/dev/", 5) != 0)
    {
        if (root) cJSON_Delete(root);
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"串口路径必须是有效的 /dev/ 设备路径\"}");
        return;
    }

    int new_fd = uart_init(serial_path->valuestring, GATEWAY_BAUD_RATE);
    if (new_fd < 0)
    {
        cJSON_Delete(root);
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"串口打开失败，请检查路径和设备连接\"}");
        return;
    }

    if (!save_config)
    {
        close(new_fd);
        cJSON_Delete(root);
        mg_http_reply(c, 200,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"success\",\"msg\":\"串口连接测试成功\"}");
        return;
    }

    pthread_mutex_lock(&g_uart_mutex);
    int old_fd = g_uart_fd;
    g_uart_fd = new_fd;
    snprintf(g_serial_path, sizeof(g_serial_path), "%s", serial_path->valuestring);
    pthread_mutex_unlock(&g_uart_mutex);
    if (old_fd >= 0) close(old_fd);

    int persisted = save_gateway_config() == 0;
    cJSON_Delete(root);
    request_network_info();
    mg_http_reply(c, persisted ? 200 : 500,
                  "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  persisted
                    ? "{\"status\":\"success\",\"msg\":\"串口路径已保存并重新连接\"}"
                    : "{\"status\":\"error\",\"msg\":\"串口已连接，但配置文件保存失败\"}");
}

static cJSON *parse_http_json(const struct mg_http_message *hm)
{
    char *body = malloc(hm->body.len + 1U);
    if (!body) return NULL;
    memcpy(body, hm->body.buf, hm->body.len);
    body[hm->body.len] = '\0';
    cJSON *root = cJSON_Parse(body);
    free(body);
    return root;
}

static int json_number_in_set(const cJSON *item, const uint32_t *values,
                              size_t count, uint32_t *result)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) return 0;
    for (size_t i = 0; i < count; i++)
    {
        if (item->valuedouble == (double)values[i])
        {
            *result = values[i];
            return 1;
        }
    }
    return 0;
}

static MonitorConfig_t load_current_monitor_config(void)
{
    MonitorConfig_t cfg = {
        MONITOR_DEFAULT_AUTO_INTERVAL_SEC,
        MONITOR_DEFAULT_POLL_INTERVAL_SEC,
        MONITOR_DEFAULT_VIBRATION_RATE_HZ,
        MONITOR_DEFAULT_DIGITAL_MIC_RATE_HZ,
        MONITOR_DEFAULT_ANALOG_MIC_RATE_HZ,
        MONITOR_DEFAULT_VIBRATION_WINDOW_MS,
        MONITOR_DEFAULT_DIGITAL_MIC_WINDOW_MS,
        MONITOR_DEFAULT_ANALOG_MIC_WINDOW_MS,
        MONITOR_DEFAULT_RAW_UPLOAD_EVERY_CYCLES,
        1U,
        MONITOR_DEFAULT_POST_UPLOAD_RX_WINDOW_SEC,
        MONITOR_SENSOR_ALL
    };
    load_monitor_config(&cfg.auto_interval_sec, &cfg.poll_interval_sec,
                        &cfg.vibration_rate_hz, &cfg.digital_mic_rate_hz,
                        &cfg.analog_mic_rate_hz, &cfg.vibration_window_ms,
                        &cfg.digital_mic_window_ms, &cfg.analog_mic_window_ms,
                        &cfg.post_upload_rx_window_sec);
    MonitorPolicyRecord_t *policies = NULL;
    int count = get_monitor_policies(&policies);
    for (int i = 0; i < count; ++i)
    {
        if (!policies[i].is_default) continue;
        cfg.auto_interval_sec = policies[i].auto_interval_sec;
        cfg.poll_interval_sec = policies[i].poll_interval_sec;
        cfg.vibration_rate_hz = policies[i].vibration_rate_hz;
        cfg.digital_mic_rate_hz = policies[i].digital_mic_rate_hz;
        cfg.analog_mic_rate_hz = policies[i].analog_mic_rate_hz;
        cfg.vibration_window_ms = policies[i].vibration_window_ms;
        cfg.digital_mic_window_ms = policies[i].digital_mic_window_ms;
        cfg.analog_mic_window_ms = policies[i].analog_mic_window_ms;
        cfg.raw_upload_every_cycles = policies[i].raw_upload_every_cycles;
        cfg.post_upload_rx_window_sec = policies[i].post_upload_rx_window_sec;
        cfg.sensor_enable_mask = policies[i].sensor_enable_mask;
        break;
    }
    free(policies);
    return cfg;
}

static MonitorConfig_t load_monitor_config_for_mac(const char *mac)
{
    MonitorConfig_t cfg = load_current_monitor_config();
    MonitorPolicyRecord_t policy;
    uint32_t generation = 1U;
    if (mac && load_device_monitor_policy(mac, &policy, &generation) == 0)
    {
        cfg.auto_interval_sec = policy.auto_interval_sec;
        cfg.poll_interval_sec = policy.poll_interval_sec;
        cfg.vibration_rate_hz = policy.vibration_rate_hz;
        cfg.digital_mic_rate_hz = policy.digital_mic_rate_hz;
        cfg.analog_mic_rate_hz = policy.analog_mic_rate_hz;
        cfg.vibration_window_ms = policy.vibration_window_ms;
        cfg.digital_mic_window_ms = policy.digital_mic_window_ms;
        cfg.analog_mic_window_ms = policy.analog_mic_window_ms;
        cfg.raw_upload_every_cycles = policy.raw_upload_every_cycles;
        cfg.raw_upload_generation = generation;
        cfg.post_upload_rx_window_sec = policy.post_upload_rx_window_sec;
        cfg.sensor_enable_mask = policy.sensor_enable_mask;
    }
    return cfg;
}

void api_handle_monitor_settings(struct mg_connection *c, struct mg_http_message *hm)
{
    MonitorConfig_t cfg = load_current_monitor_config();
    if (mg_strcmp(hm->method, mg_str("POST")) == 0)
    {
        static const uint32_t vibration_window_values[] = {500, 1000, 1500};
        static const uint32_t microphone_window_values[] = {500, 1000, 1500, 2000};
        cJSON *root = parse_http_json(hm);
        uint32_t vibration_window = 0, digital_window = 0, analog_window = 0;
        cJSON *auto_interval = root ? cJSON_GetObjectItem(root, "auto_interval_sec") : NULL;
        cJSON *legacy_window = root ? cJSON_GetObjectItem(root, "mic_window_ms") : NULL;
        cJSON *vibration_window_item = root ? cJSON_GetObjectItem(root, "vibration_window_ms") : NULL;
        cJSON *digital_window_item = root ? cJSON_GetObjectItem(root, "digital_mic_window_ms") : NULL;
        cJSON *analog_window_item = root ? cJSON_GetObjectItem(root, "analog_mic_window_ms") : NULL;
        if (!vibration_window_item) vibration_window_item = legacy_window;
        if (!digital_window_item) digital_window_item = legacy_window;
        if (!analog_window_item) analog_window_item = legacy_window;
        int valid = root && cJSON_IsNumber(auto_interval) &&
                    auto_interval->valuedouble >= MONITOR_MIN_AUTO_INTERVAL_SEC &&
                    auto_interval->valuedouble <= 86400 &&
                    json_number_in_set(vibration_window_item, vibration_window_values, 3, &vibration_window) &&
                    json_number_in_set(digital_window_item, microphone_window_values, 4, &digital_window) &&
                    json_number_in_set(analog_window_item, microphone_window_values, 4, &analog_window);
        if (!valid)
        {
            cJSON_Delete(root);
            mg_http_reply(c, 400, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                          "{\"status\":\"error\",\"msg\":\"Invalid monitoring settings\"}");
            return;
        }
        cfg.auto_interval_sec = (uint32_t)auto_interval->valuedouble;
        cfg.poll_interval_sec = MONITOR_DEFAULT_POLL_INTERVAL_SEC; /* 协议兼容保留，不再周期轮询 */
        cfg.vibration_rate_hz = MONITOR_DEFAULT_VIBRATION_RATE_HZ;
        cfg.digital_mic_rate_hz = MONITOR_DEFAULT_DIGITAL_MIC_RATE_HZ;
        cfg.analog_mic_rate_hz = MONITOR_DEFAULT_ANALOG_MIC_RATE_HZ;
        cfg.vibration_window_ms = (uint16_t)vibration_window;
        cfg.digital_mic_window_ms = (uint16_t)digital_window;
        cfg.analog_mic_window_ms = (uint16_t)analog_window;
        cJSON_Delete(root);
        if (save_monitor_config(cfg.auto_interval_sec, cfg.poll_interval_sec,
                                cfg.vibration_rate_hz, cfg.digital_mic_rate_hz,
                                cfg.analog_mic_rate_hz, cfg.vibration_window_ms,
                                cfg.digital_mic_window_ms, cfg.analog_mic_window_ms,
                                cfg.post_upload_rx_window_sec) != 0)
        {
            mg_http_reply(c, 500, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                          "{\"status\":\"error\",\"msg\":\"Database update failed\"}");
            return;
        }
        printf("[监测策略] 已保存；后端将在节点状态上报时推送新参数。\n");
    }

    mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "{\"status\":\"success\",\"auto_interval_sec\":%u,"
                  "\"poll_interval_sec\":%u,\"vibration_rate_hz\":%u,"
                  "\"digital_mic_rate_hz\":%u,\"analog_mic_rate_hz\":%u,"
                  "\"vibration_window_ms\":%u,\"digital_mic_window_ms\":%u,"
                  "\"analog_mic_window_ms\":%u,\"mic_window_ms\":%u}",
                  cfg.auto_interval_sec, cfg.poll_interval_sec, cfg.vibration_rate_hz,
                  cfg.digital_mic_rate_hz, cfg.analog_mic_rate_hz,
                  cfg.vibration_window_ms, cfg.digital_mic_window_ms,
                  cfg.analog_mic_window_ms, cfg.digital_mic_window_ms);
}

static int parse_monitor_policy_json(cJSON *root, MonitorPolicyRecord_t *policy,
                                     const char ***node_macs, int *node_count)
{
    static const uint32_t vibration_window_values[] = {500, 1000, 1500};
    static const uint32_t microphone_window_values[] = {500, 1000, 1500, 2000};
    uint32_t vibration_window = 0, digital_window = 0, analog_window = 0;
    memset(policy, 0, sizeof(*policy));
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *auto_interval = cJSON_GetObjectItem(root, "auto_interval_sec");
    cJSON *vibration_window_item = cJSON_GetObjectItem(root, "vibration_window_ms");
    cJSON *digital_window_item = cJSON_GetObjectItem(root, "digital_mic_window_ms");
    cJSON *analog_window_item = cJSON_GetObjectItem(root, "analog_mic_window_ms");
    cJSON *raw_cycles = cJSON_GetObjectItem(root, "raw_upload_every_cycles");
    cJSON *vibration_enabled = cJSON_GetObjectItem(root, "vibration_enabled");
    cJSON *digital_mic_enabled = cJSON_GetObjectItem(root, "digital_microphone_enabled");
    cJSON *analog_mic_enabled = cJSON_GetObjectItem(root, "analog_microphone_enabled");
    cJSON *nodes = cJSON_GetObjectItem(root, "node_macs");
    if (!cJSON_IsString(name) || !name->valuestring ||
        normalize_custom_name(name->valuestring, policy->name,
                              sizeof(policy->name)) != 0 ||
        policy->name[0] == '\0' ||
        !cJSON_IsNumber(auto_interval) ||
        auto_interval->valuedouble < MONITOR_MIN_AUTO_INTERVAL_SEC ||
        auto_interval->valuedouble > 86400 ||
        auto_interval->valuedouble != (uint32_t)auto_interval->valuedouble ||
        !json_number_in_set(vibration_window_item, vibration_window_values, 3, &vibration_window) ||
        !json_number_in_set(digital_window_item, microphone_window_values, 4, &digital_window) ||
        !json_number_in_set(analog_window_item, microphone_window_values, 4, &analog_window) ||
        !cJSON_IsNumber(raw_cycles) || raw_cycles->valuedouble < 1 ||
        raw_cycles->valuedouble > 65535 ||
        raw_cycles->valuedouble != (uint32_t)raw_cycles->valuedouble || !cJSON_IsArray(nodes) ||
        (vibration_enabled && !cJSON_IsBool(vibration_enabled)) ||
        (digital_mic_enabled && !cJSON_IsBool(digital_mic_enabled)) ||
        (analog_mic_enabled && !cJSON_IsBool(analog_mic_enabled))) return 0;

    policy->id = cJSON_IsNumber(id) ? id->valueint : 0;
    policy->auto_interval_sec = (uint32_t)auto_interval->valuedouble;
    policy->poll_interval_sec = MONITOR_DEFAULT_POLL_INTERVAL_SEC;
    policy->vibration_rate_hz = MONITOR_DEFAULT_VIBRATION_RATE_HZ;
    policy->digital_mic_rate_hz = MONITOR_DEFAULT_DIGITAL_MIC_RATE_HZ;
    policy->analog_mic_rate_hz = MONITOR_DEFAULT_ANALOG_MIC_RATE_HZ;
    policy->vibration_window_ms = (uint16_t)vibration_window;
    policy->digital_mic_window_ms = (uint16_t)digital_window;
    policy->analog_mic_window_ms = (uint16_t)analog_window;
    policy->raw_upload_every_cycles = (uint16_t)raw_cycles->valuedouble;
    /* Retain the legacy database column only for schema compatibility. */
    policy->post_upload_rx_window_sec = MONITOR_DEFAULT_POST_UPLOAD_RX_WINDOW_SEC;
    policy->sensor_enable_mask =
        ((!vibration_enabled || cJSON_IsTrue(vibration_enabled)) ? MONITOR_SENSOR_VIBRATION : 0U) |
        ((!digital_mic_enabled || cJSON_IsTrue(digital_mic_enabled)) ? MONITOR_SENSOR_DIGITAL_MIC : 0U) |
        ((!analog_mic_enabled || cJSON_IsTrue(analog_mic_enabled)) ? MONITOR_SENSOR_ANALOG_MIC : 0U);

    int count = cJSON_GetArraySize(nodes);
    const char **macs = count > 0 ? calloc((size_t)count, sizeof(*macs)) : NULL;
    if (count > 0 && !macs) return 0;
    for (int i = 0; i < count; ++i)
    {
        cJSON *item = cJSON_GetArrayItem(nodes, i);
        if (!cJSON_IsString(item) || !item->valuestring ||
            strlen(item->valuestring) >= 32U)
        {
            free(macs);
            return 0;
        }
        macs[i] = item->valuestring;
    }
    *node_macs = macs;
    *node_count = count;
    return 1;
}

static cJSON *device_sampling_json(const MonitorPolicyRecord_t *policy);
static void api_handle_node_groups(struct mg_connection *c, struct mg_http_message *hm, int deleting)
{
    const char *headers = "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n";
    if (!deleting && mg_strcmp(hm->method, mg_str("GET")) == 0) {
        NodeGroupRecord_t *groups = NULL;
        int count = get_node_groups(&groups);
        cJSON *body = cJSON_CreateObject();
        cJSON *array = cJSON_AddArrayToObject(body, "groups");
        int failed = count < 0;
        for (int i = 0; i < count && !failed; ++i) {
            char (*macs)[32] = NULL;
            int members = get_node_group_members(groups[i].id, &macs);
            if (members < 0) { failed = 1; free(macs); break; }
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", groups[i].id);
            cJSON_AddStringToObject(item, "name", groups[i].name);
            cJSON_AddStringToObject(item, "description", groups[i].description);
            MonitorPolicyRecord_t config;
            int applied = get_node_group_sampling(groups[i].id, &config);
            if (applied < 0) { free(macs); cJSON_Delete(item); failed = 1; break; }
            cJSON_AddBoolToObject(item, "sampling_applied", applied == 1);
            cJSON_AddItemToObject(item, "sampling_config", applied ? device_sampling_json(&config) : cJSON_CreateNull());
            cJSON *list = cJSON_AddArrayToObject(item, "node_macs");
            for (int j = 0; j < members; ++j) cJSON_AddItemToArray(list, cJSON_CreateString(macs[j]));
            free(macs); cJSON_AddItemToArray(array, item);
        }
        cJSON_AddStringToObject(body, "status", failed ? "error" : "success");
        char *json = cJSON_PrintUnformatted(body);
        mg_http_reply(c, failed ? 500 : 200, headers, "%s", json ? json : "{}");
        free(json); free(groups); cJSON_Delete(body); return;
    }
    if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
        mg_http_reply(c, 405, headers, "{\"status\":\"error\",\"msg\":\"Method not allowed\"}"); return;
    }
    cJSON *body = parse_http_json(hm);
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(body, "id");
    int id = cJSON_IsNumber(id_item) ? id_item->valueint : 0;
    int valid = body && (!id_item || (cJSON_IsNumber(id_item) && id >= 0 && id_item->valuedouble == id));
    int result = 2;
    if (deleting == 2) {
        MonitorPolicyRecord_t policy;
        const char **unused_macs = NULL;
        int unused_count = 0, updated = 0;
        /* Group identity is captured above; policy identity/member list are server-owned. */
        cJSON_DeleteItemFromObject(body, "id");
        cJSON_DeleteItemFromObject(body, "name");
        cJSON_DeleteItemFromObject(body, "node_macs");
        if (body) {
            cJSON_AddStringToObject(body, "name", "分组应用采样配置");
            cJSON_AddArrayToObject(body, "node_macs");
        }
        if (valid && id > 0 && parse_monitor_policy_json(body, &policy, &unused_macs, &unused_count))
            result = apply_node_group_sampling(id, &policy, &updated);
        free(unused_macs);
        if (!result) {
            gateway_log_event("INFO", "frontend", "group_sampling_saved", NULL, -1, -1,
                              "分组 ID=%d 采样参数已保存，更新 %d 个节点；将在节点状态上报时推送", id, updated);
        }
    } else if (deleting) {
        if (valid && id > 0) result = delete_node_group(id) == 0 ? 0 : 2;
    } else {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(body, "name");
        cJSON *description = cJSON_GetObjectItemCaseSensitive(body, "description");
        cJSON *members = cJSON_GetObjectItemCaseSensitive(body, "node_macs");
        int count = cJSON_GetArraySize(members);
        valid = valid && cJSON_IsString(name) && name->valuestring[0] &&
            strlen(name->valuestring) <= 128 && cJSON_IsString(description) &&
            strlen(description->valuestring) <= 768 && cJSON_IsArray(members) && count <= 4096;
        const char **macs = valid && count ? calloc((size_t)count, sizeof(*macs)) : NULL;
        if (valid && count && !macs) { valid = 0; result = -1; }
        for (int i = 0; i < count && valid; ++i) {
            cJSON *item = cJSON_GetArrayItem(members, i);
            if (!cJSON_IsString(item) || !item->valuestring[0] || strlen(item->valuestring) >= 32) valid = 0;
            else macs[i] = item->valuestring;
        }
        if (valid) result = save_node_group(&id, name->valuestring, description->valuestring, macs, count);
        free(macs);
    }
    cJSON_Delete(body);
    if (!result) mg_http_reply(c, 200, headers, "{\"status\":\"success\",\"id\":%d}", id);
    else mg_http_reply(c, result == 1 ? 409 : (result < 0 ? 500 : 400), headers,
        "{\"status\":\"error\",\"msg\":\"%s\"}", result == 1 ? "节点已属于其他分组，请先从原组移除" : (result < 0 ? "数据库操作失败" : "参数无效，或分组/节点已不存在"));
}

void api_handle_monitor_policies(struct mg_connection *c,
                                 struct mg_http_message *hm)
{
    if (mg_strcmp(hm->method, mg_str("GET")) == 0)
    {
        MonitorPolicyRecord_t *policies = NULL;
        DeviceRecord_t *devices = NULL;
        int policy_count = get_monitor_policies(&policies);
        int device_count = get_confirmed_devices(&devices);
        if (policy_count < 0 || device_count < 0)
        {
            free(policies); free(devices);
            mg_http_reply(c, 500, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                          "{\"status\":\"error\",\"msg\":\"Database query failed\"}");
            return;
        }
        cJSON *root = cJSON_CreateObject();
        cJSON *array = cJSON_AddArrayToObject(root, "policies");
        cJSON_AddStringToObject(root, "status", "success");
        for (int i = 0; i < policy_count; ++i)
        {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", policies[i].id);
            cJSON_AddStringToObject(item, "name", policies[i].name);
            cJSON_AddBoolToObject(item, "is_default", policies[i].is_default);
            cJSON_AddNumberToObject(item, "auto_interval_sec", policies[i].auto_interval_sec);
            cJSON_AddNumberToObject(item, "vibration_rate_hz", policies[i].vibration_rate_hz);
            cJSON_AddNumberToObject(item, "digital_mic_rate_hz", policies[i].digital_mic_rate_hz);
            cJSON_AddNumberToObject(item, "analog_mic_rate_hz", policies[i].analog_mic_rate_hz);
            cJSON_AddNumberToObject(item, "vibration_window_ms", policies[i].vibration_window_ms);
            cJSON_AddNumberToObject(item, "digital_mic_window_ms", policies[i].digital_mic_window_ms);
            cJSON_AddNumberToObject(item, "analog_mic_window_ms", policies[i].analog_mic_window_ms);
            cJSON_AddNumberToObject(item, "raw_upload_every_cycles", policies[i].raw_upload_every_cycles);
            cJSON_AddBoolToObject(item, "vibration_enabled",
                                  (policies[i].sensor_enable_mask & MONITOR_SENSOR_VIBRATION) != 0U);
            cJSON_AddBoolToObject(item, "digital_microphone_enabled",
                                  (policies[i].sensor_enable_mask & MONITOR_SENSOR_DIGITAL_MIC) != 0U);
            cJSON_AddBoolToObject(item, "analog_microphone_enabled",
                                  (policies[i].sensor_enable_mask & MONITOR_SENSOR_ANALOG_MIC) != 0U);
            cJSON *nodes = cJSON_AddArrayToObject(item, "node_macs");
            for (int j = 0; j < device_count; ++j)
                if (devices[j].monitor_policy_id == policies[i].id)
                    cJSON_AddItemToArray(nodes, cJSON_CreateString(devices[j].mac));
            cJSON_AddItemToArray(array, item);
        }
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root); free(policies); free(devices);
        mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n", "%s", json);
        free(json);
        return;
    }

    if (mg_strcmp(hm->method, mg_str("POST")) == 0)
    {
        cJSON *root = parse_http_json(hm);
        MonitorPolicyRecord_t policy;
        const char **node_macs = NULL;
        int node_count = 0, policy_id = 0;
        if (!root || !parse_monitor_policy_json(root, &policy, &node_macs, &node_count))
        {
            free(node_macs); cJSON_Delete(root);
            mg_http_reply(c, 400, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                          "{\"status\":\"error\",\"msg\":\"Invalid monitoring policy\"}");
            return;
        }
        int result = save_monitor_policy(&policy, node_macs, node_count, &policy_id);
        free(node_macs); cJSON_Delete(root);
        if (result != 0)
        {
            mg_http_reply(c, 500, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                          "{\"status\":\"error\",\"msg\":\"Policy save failed\"}");
            return;
        }
        mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"success\",\"id\":%d}", policy_id);
        return;
    }
    mg_http_reply(c, 405, "Access-Control-Allow-Origin: *\r\n", "Method Not Allowed");
}

void api_handle_monitor_policy_delete(struct mg_connection *c,
                                      struct mg_http_message *hm)
{
    cJSON *root = parse_http_json(hm);
    cJSON *id = root ? cJSON_GetObjectItem(root, "id") : NULL;
    int policy_id = cJSON_IsNumber(id) ? id->valueint : 0;
    cJSON_Delete(root);
    if (policy_id <= 1 || delete_monitor_policy(policy_id) != 0)
    {
        mg_http_reply(c, 400, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Policy cannot be deleted\"}");
        return;
    }
    mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "{\"status\":\"success\"}");
}

static int parse_device_mac(struct mg_http_message *hm, char mac[32], cJSON **root_out)
{
    cJSON *root = parse_http_json(hm);
    cJSON *item = root ? cJSON_GetObjectItem(root, "mac") : NULL;
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) >= 32U)
    {
        cJSON_Delete(root);
        return 0;
    }
    strcpy(mac, item->valuestring);
    *root_out = root;
    return 1;
}

static cJSON *device_sampling_json(const MonitorPolicyRecord_t *policy)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "id", policy->id);
    cJSON_AddNumberToObject(item, "auto_interval_sec", policy->auto_interval_sec);
    cJSON_AddNumberToObject(item, "vibration_rate_hz", policy->vibration_rate_hz);
    cJSON_AddNumberToObject(item, "digital_mic_rate_hz", policy->digital_mic_rate_hz);
    cJSON_AddNumberToObject(item, "analog_mic_rate_hz", policy->analog_mic_rate_hz);
    cJSON_AddNumberToObject(item, "vibration_window_ms", policy->vibration_window_ms);
    cJSON_AddNumberToObject(item, "digital_mic_window_ms", policy->digital_mic_window_ms);
    cJSON_AddNumberToObject(item, "analog_mic_window_ms", policy->analog_mic_window_ms);
    cJSON_AddNumberToObject(item, "raw_upload_every_cycles", policy->raw_upload_every_cycles);
    cJSON_AddBoolToObject(item, "vibration_enabled", (policy->sensor_enable_mask & MONITOR_SENSOR_VIBRATION) != 0);
    cJSON_AddBoolToObject(item, "digital_microphone_enabled", (policy->sensor_enable_mask & MONITOR_SENSOR_DIGITAL_MIC) != 0);
    cJSON_AddBoolToObject(item, "analog_microphone_enabled", (policy->sensor_enable_mask & MONITOR_SENSOR_ANALOG_MIC) != 0);
    return item;
}

/* Save desired settings; synchronize on A4 receive-window reports. Saving
 * ordinary parameters never starts unlimited radio retries while asleep. */
void api_handle_device_sampling(struct mg_connection *c, struct mg_http_message *hm)
{
    const char *headers = "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n";
    char mac[32] = {0};
    cJSON *body = NULL;
    int saving = mg_strcmp(hm->method, mg_str("POST")) == 0;
    int reading = mg_strcmp(hm->method, mg_str("GET")) == 0;
    if (!saving && !reading) { mg_http_reply(c, 405, headers, "{\"status\":\"error\",\"msg\":\"Method not allowed\"}"); return; }
    if ((saving && !parse_device_mac(hm, mac, &body)) ||
        (reading && (mg_http_get_var(&hm->query, "mac", mac, sizeof(mac)) <= 0))) {
        mg_http_reply(c, 400, headers, "{\"status\":\"error\",\"msg\":\"Missing or invalid MAC\"}"); return;
    }
    MonitorPolicyRecord_t policy;
    uint32_t generation = 0;
    if (load_device_monitor_policy(mac, &policy, &generation) != 0) {
        cJSON_Delete(body);
        mg_http_reply(c, 404, headers, "{\"status\":\"error\",\"msg\":\"Node not found\"}"); return;
    }
    if (saving) {
        /* Accept settings for one MAC only; browser ids and node lists cannot
         * redirect this operation to another device or a shared policy. */
        cJSON_DeleteItemFromObject(body, "id");
        cJSON_DeleteItemFromObject(body, "name");
        cJSON_DeleteItemFromObject(body, "node_macs");
        cJSON_AddStringToObject(body, "name", "节点独立采样配置");
        cJSON *nodes = cJSON_AddArrayToObject(body, "node_macs");
        cJSON_AddItemToArray(nodes, cJSON_CreateString(mac));
        const char **parsed_nodes = NULL;
        int count = 0, id = 0;
        int valid = parse_monitor_policy_json(body, &policy, &parsed_nodes, &count);
        free(parsed_nodes);
        if (!valid) {
            cJSON_Delete(body);
            mg_http_reply(c, 400, headers, "{\"status\":\"error\",\"msg\":\"Invalid sampling settings\"}"); return;
        }
        int result = save_device_monitor_policy(mac, &policy, &id);
        cJSON_Delete(body);
        if (result != 0 || load_device_monitor_policy(mac, &policy, &generation) != 0) {
            mg_http_reply(c, 500, headers, "{\"status\":\"error\",\"msg\":\"Sampling settings save failed\"}"); return;
        }
        gateway_log_event("INFO", "frontend", "sampling_saved", mac, -1, -1,
                          "独立采样配置已保存：周期=%us，振动=%ums；等待节点上传完成或异常后同步", policy.auto_interval_sec, policy.vibration_window_ms);
        printf("✅ [节点采样配置] MAC=%s | 独立配置=%d | 周期=%us | 振动=%ums | 已保存，等待节点上传完成或异常后同步\n",
               mac, id, policy.auto_interval_sec, policy.vibration_window_ms);
    }
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "mac", mac);
    cJSON_AddItemToObject(response, "config", device_sampling_json(&policy));
    cJSON_AddNumberToObject(response, "raw_upload_generation", generation);
    if (saving) cJSON_AddStringToObject(response, "delivery", "push_pending_confirmation");
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    mg_http_reply(c, 200, headers, "%s", json);
    free(json);
}

void api_handle_device_monitor(struct mg_connection *c, struct mg_http_message *hm)
{
    char mac[32] = {0};
    cJSON *root = NULL;
    if (!parse_device_mac(hm, mac, &root))
    {
        mg_http_reply(c, 400, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Missing mac\"}");
        return;
    }
    cJSON *enabled_item = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(enabled_item))
    {
        cJSON_Delete(root);
        mg_http_reply(c, 400, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Missing enabled\"}");
        return;
    }
    int enabled = cJSON_IsTrue(enabled_item) ? 1 : 0;
    cJSON_Delete(root);
    int found = is_device_in_db(mac);
    if (!found || update_device_monitoring(mac, enabled) != 0)
    {
        mg_http_reply(c, found ? 500 : 404, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Node missing or setting could not be saved\"}");
        return;
    }
    uint16_t address;
    int submitted=get_device_short_addr(mac,&address) &&
                  send_monitor_control(address,MONITOR_OP_SET_ENABLED,enabled);
    gateway_log_event("INFO", "frontend", "monitor_switch", mac, -1, -1,
                      "在线监测设置已保存：%s；主动下发%s，节点状态上报时检查是否需要重发",
                      enabled ? "开启" : "关闭",submitted?"已提交":"待重试");
    pthread_mutex_lock(&g_ws_mutex);
    snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer),
             "{\"type\":\"monitor_setting\",\"mac\":\"%s\",\"enabled\":%s}", mac, enabled ? "true" : "false");
    queue_ws_event_locked();
    pthread_mutex_unlock(&g_ws_mutex);
    mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "{\"status\":\"success\",\"enabled\":%s}", enabled ? "true" : "false");
}

void api_handle_device_collect(struct mg_connection *c, struct mg_http_message *hm)
{
    char mac[32] = {0};
    cJSON *root = NULL;
    if (!parse_device_mac(hm, mac, &root))
    {
        mg_http_reply(c, 400, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Missing mac\"}");
        return;
    }
    cJSON_Delete(root);
    uint16_t short_addr = 0;
    int found = get_device_short_addr(mac, &short_addr);
    if (!found || !send_monitor_control(short_addr, MONITOR_OP_COLLECT_NOW, 0))
    {
        mg_http_reply(c, found ? 503 : 404, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Node or coordinator unavailable\"}");
        return;
    }
    update_device_collection_state(mac, "waiting", 0);
    mg_http_reply(c, 202, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "{\"status\":\"accepted\"}");
}

void api_handle_monitor_all(struct mg_connection *c, struct mg_http_message *hm)
{
    cJSON *root = parse_http_json(hm);
    cJSON *enabled_item = root ? cJSON_GetObjectItem(root, "enabled") : NULL;
    if (!cJSON_IsBool(enabled_item))
    {
        cJSON_Delete(root);
        mg_http_reply(c, 400, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Missing enabled\"}");
        return;
    }
    int enabled = cJSON_IsTrue(enabled_item);
    cJSON_Delete(root);
    DeviceRecord_t *records = NULL;
    int count = get_confirmed_devices(&records);
    int sent = 0;
    for (int i = 0; i < count; i++)
    {
        /* 开关意图写入所有白名单设备；离线节点在重连时自动恢复该状态。 */
        update_device_monitoring(records[i].mac, enabled);
        update_device_collection_state(records[i].mac, enabled ? "waiting" : "idle", 0);
        if (strcmp(records[i].status, "online") != 0)
        {
            continue;
        }
        unsigned int short_addr = 0;
        if (sscanf(records[i].short_addr, "%x", &short_addr) == 1)
        {
            sent += send_monitor_control((uint16_t)short_addr, MONITOR_OP_SET_ENABLED, enabled);
            sleep_microseconds(20000U);
        }
    }
    free(records);
    mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "{\"status\":\"success\",\"sent\":%d,\"enabled\":%s}",
                  sent, enabled ? "true" : "false");
}

void api_handle_collect_all(struct mg_connection *c)
{
    DeviceRecord_t *records = NULL;
    int count = get_confirmed_devices(&records);
    int sent = 0;
    for (int i = 0; i < count; i++)
    {
        if (strcmp(records[i].status, "online") != 0)
        {
            continue;
        }
        unsigned int short_addr = 0;
        if (sscanf(records[i].short_addr, "%x", &short_addr) == 1)
        {
            if (send_monitor_control((uint16_t)short_addr, MONITOR_OP_COLLECT_NOW, 0))
            {
                sent++;
                update_device_collection_state(records[i].mac, "waiting", 0);
            }
            sleep_microseconds(20000U);
        }
    }
    free(records);
    mg_http_reply(c, 202, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "{\"status\":\"accepted\",\"sent\":%d}", sent);
}

static cJSON *vibration_batch_json(const char *mac, uint32_t requested_batch,
                                  int completed_only, int include_raw)
{
    uint32_t batch_id = 0;
    uint32_t rate = 0, count = 0;
    int16_t *samples = NULL;
    size_t bytes = 0;
    int32_t features[3][5] = {{0}};
    long long timestamp = 0;
    if (!get_vibration_batch(mac, requested_batch, completed_only, &batch_id, &rate, &count,
                                    (void **)&samples, &bytes, features, &timestamp))
        return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "batch_id", batch_id);
    cJSON_AddNumberToObject(root, "sample_rate_hz", rate);
    cJSON_AddNumberToObject(root, "sample_count", count);
    cJSON_AddNumberToObject(root, "timestamp", (double)timestamp);
    cJSON_AddNumberToObject(root, "captured_at", (double)get_collection_capture_time(mac, batch_id));
    int raw_complete = count > 0 && bytes == (size_t)count * 3U * sizeof(int16_t);
    cJSON_AddStringToObject(root, "raw_upload_status",
                           raw_complete ? "complete" : "uploading");
    cJSON_AddBoolToObject(root, "raw_available", raw_complete);
    int upload_started = bytes > 0;
    /* Detail refresh must not reset a V2 window progress bar to zero. */
    pthread_mutex_lock(&g_vibration_mutex);
    for (unsigned i=0;i<V2_GATEWAY_SLOTS;++i)
    {
        const V2GatewaySlot *slot=&v2_gateway_slots[i];
        if (slot->rx.active && slot->batch_id==batch_id && !strcmp(slot->mac,mac))
        {
            upload_started |= slot->scalar_started || slot->last_fragment_ms != 0;
            cJSON_AddNumberToObject(root,"raw_received_packets",v2_rx_done_count(&slot->rx));
            cJSON_AddNumberToObject(root,"raw_total_packets",v2_blocks(count));
            cJSON_AddStringToObject(root,"raw_progress_unit","blocks");
            break;
        }
    }
    pthread_mutex_unlock(&g_vibration_mutex);
    float batch_temp = 0.0f, batch_mag_x = 0.0f, batch_mag_y = 0.0f,
          batch_mag_z = 0.0f, batch_digital = 0.0f, batch_analog = 0.0f;
    unsigned int value_mask = 0U;
    if (get_collection_values(mac, batch_id, &batch_temp, &batch_mag_x,
                              &batch_mag_y, &batch_mag_z, &batch_digital,
                              &batch_analog, &value_mask))
    {
        cJSON *values = cJSON_AddObjectToObject(root, "collection_values");
        if (value_mask & (1U << 0)) cJSON_AddNumberToObject(values, "temperature", batch_temp);
        if ((value_mask & 0x0EU) == 0x0EU)
        {
            cJSON *magnetic = cJSON_AddObjectToObject(values, "magnetic");
            cJSON_AddNumberToObject(magnetic, "x", batch_mag_x);
            cJSON_AddNumberToObject(magnetic, "y", batch_mag_y);
            cJSON_AddNumberToObject(magnetic, "z", batch_mag_z);
        }
        if (value_mask & (1U << 4)) cJSON_AddNumberToObject(values, "digital_db", batch_digital);
        if (value_mask & (1U << 5)) cJSON_AddNumberToObject(values, "analog_db", batch_analog);
    }
    cJSON *feature_root = cJSON_AddArrayToObject(root, "features");
    int features_ready = !v2_features_pending(features);
    cJSON_AddBoolToObject(root, "features_ready", features_ready);
    cJSON_AddBoolToObject(root, "upload_started", upload_started || value_mask != 0 || features_ready);
    for (size_t axis = 0; features_ready && axis < 3; axis++)
    {
        cJSON *row = cJSON_CreateArray();
        for (size_t feature = 0; feature < 5; feature++)
            cJSON_AddItemToArray(row, cJSON_CreateNumber(features[axis][feature] / 1000.0));
        cJSON_AddItemToArray(feature_root, row);
    }
    cJSON *data = cJSON_AddArrayToObject(root, "samples");
    uint32_t step = include_raw ? 1U : (count > 1200U ? (count + 1199U) / 1200U : 1U);
    cJSON_AddNumberToObject(root, "display_step", step);
    if (raw_complete)
    {
        for (uint32_t i = 0; i < count; i += step)
        {
            cJSON *sample = cJSON_CreateArray();
            cJSON_AddItemToArray(sample, cJSON_CreateNumber(samples[i * 3U]));
            cJSON_AddItemToArray(sample, cJSON_CreateNumber(samples[i * 3U + 1U]));
            cJSON_AddItemToArray(sample, cJSON_CreateNumber(samples[i * 3U + 2U]));
            cJSON_AddItemToArray(data, sample);
        }
    }
    free(samples);
    return root;
}

void api_handle_vibration_latest(struct mg_connection *c, struct mg_http_message *hm)
{
    const char *headers="Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n";
    char mac[32]={0}, raw_text[8]={0}, batch_text[32]={0};
    if (mg_http_get_var(&hm->query,"mac",mac,sizeof(mac))<=0) {
        mg_http_reply(c,400,headers,"{\"status\":\"error\",\"msg\":\"Missing mac\"}");
        return;
    }
    uint32_t requested_batch=0;
    int batch_length=mg_http_get_var(&hm->query,"batch",batch_text,sizeof(batch_text));
    if (batch_length != -1) {
        char *end=NULL;
        unsigned long long value=strtoull(batch_text,&end,10);
        if (batch_length<=0 || !strspn(batch_text,"0123456789") ||
            strspn(batch_text,"0123456789")!=strlen(batch_text) ||
            !end || *end || value==0 || value>UINT32_MAX) {
            mg_http_reply(c,400,headers,"{\"status\":\"error\",\"msg\":\"Invalid batch\"}");
            return;
        }
        requested_batch=(uint32_t)value;
    }
    int include_raw=mg_http_get_var(&hm->query,"raw",raw_text,sizeof(raw_text))>0 && !strcmp(raw_text,"1");
    cJSON *root=vibration_batch_json(mac,requested_batch,0,include_raw);
    if (!root) {
        mg_http_reply(c,404,headers,"{\"status\":\"error\",\"msg\":\"No vibration batch\"}");
        return;
    }
    /* A newly granted descriptor is not a received batch. Supply the last
     * complete batch as well so a refreshed page can still display/download it. */
    if (!requested_batch && !(cJSON_IsTrue(cJSON_GetObjectItem(root,"raw_available")) &&
                              cJSON_IsTrue(cJSON_GetObjectItem(root,"features_ready")))) {
        cJSON *previous=vibration_batch_json(mac,0,1,0);
        if (previous) cJSON_AddItemToObject(root,"last_complete",previous);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n", "%s", json);
    free(json);
}

/*********************************************************************
 * HTTP 核心路由器 (只管指路)
 *********************************************************************/
static void api_handle_logs(struct mg_connection *c, struct mg_http_message *hm)
{
    const char *headers = "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n";
    if (mg_strcmp(hm->method, mg_str("GET")) != 0) {
        mg_http_reply(c, 405, headers, "{\"status\":\"error\",\"msg\":\"GET required\"}"); return;
    }
    char level[16] = {0}, source[24] = {0}, mac[32] = {0}, search[256] = {0};
    int valid = log_query_text(hm, "level", level, sizeof(level)) &&
        log_query_text(hm, "source", source, sizeof(source)) &&
        log_query_text(hm, "mac", mac, sizeof(mac)) &&
        log_query_text(hm, "q", search, sizeof(search));
    const long long page = 1;
    long long limit, from, to;
    valid = valid && log_query_number(hm, "limit", 100, 1000, &limit) &&
        (limit == 100 || limit == 200 || limit == 500 || limit == 1000) &&
        log_query_number(hm, "from", 0, 4102444800LL, &from) &&
        log_query_number(hm, "to", 0, 4102444800LL, &to) && (!to || !from || to >= from);
    if (!valid) { mg_http_reply(c, 400, headers, "{\"status\":\"error\",\"msg\":\"Invalid log filters\"}"); return; }
    GatewayLogFilter filter = {level, source, mac, search, from, to, (int)limit, 0, 1};
    GatewayLog *rows = calloc((size_t)limit, sizeof(*rows));
    int total = 0;
    int count = rows ? gateway_log_query(&filter, rows, &total) : -1;
    if (count < 0) { free(rows); mg_http_reply(c, 503, headers, "{\"status\":\"error\",\"msg\":\"Log database unavailable\"}"); return; }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddNumberToObject(root, "page", (double)page);
    cJSON_AddNumberToObject(root, "limit", (double)limit);
    cJSON_AddNumberToObject(root, "dropped", (double)gateway_log_dropped());
    cJSON_AddNumberToObject(root, "retention_days", 7);
    cJSON_AddNumberToObject(root, "capacity", 50000);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < count; ++i) {
        GatewayLog *r = &rows[i]; cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", (double)r->id);
        cJSON_AddNumberToObject(item, "timestamp", (double)r->timestamp);
        cJSON_AddStringToObject(item, "level", r->level);
        cJSON_AddStringToObject(item, "source", r->source);
        cJSON_AddStringToObject(item, "event", r->event);
        cJSON_AddStringToObject(item, "mac", r->mac);
        cJSON_AddStringToObject(item, "message", r->message);
        if (r->short_addr < 0) cJSON_AddNullToObject(item, "short_addr"); else cJSON_AddNumberToObject(item, "short_addr", r->short_addr);
        if (r->batch < 0) cJSON_AddNullToObject(item, "batch"); else cJSON_AddNumberToObject(item, "batch", (double)r->batch);
        cJSON_AddItemToArray(items, item);
    }
    free(rows); char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    mg_http_reply(c, json ? 200 : 500, headers, "%s", json ? json : "{\"status\":\"error\"}"); free(json);
}

#include "manual_admission_api.h"

void route_http_requests(struct mg_connection *c, struct mg_http_message *hm)
{
    if(gateway_mqtt_api_handle(&g_mqtt_api,c,hm))return;
    // 1. 跨域放行
    if (mg_strcmp(hm->method, mg_str("OPTIONS")) == 0) 
    {
        mg_http_reply(c, 200, "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n", "");
        return;
    }
    // 2. WebSocket 升级
    if (mg_strcmp(hm->uri, mg_str("/api/ws")) == 0) 
    {
        mg_ws_upgrade(c, hm, NULL);
        return;
    }

    // 3. API 路由分发

    if (mg_strcmp(hm->uri, mg_str("/api/logs")) == 0) { api_handle_logs(c, hm); return; }
    if (mg_strcmp(hm->uri, mg_str("/api/groups")) == 0) { api_handle_node_groups(c, hm, 0); return; }
    if (mg_strcmp(hm->uri, mg_str("/api/groups/delete")) == 0) { api_handle_node_groups(c, hm, 1); return; }
    if (mg_strcmp(hm->uri, mg_str("/api/groups/sampling")) == 0) { api_handle_node_groups(c, hm, 2); return; }
    if (mg_strcmp(hm->uri, mg_str("/api/device/monitor")) == 0 && mg_strcmp(hm->method, mg_str("POST")) == 0) {
        mg_http_reply(c,409,join_headers,"{\"status\":\"error\",\"msg\":\"授权节点自动在线监测，无需单独开关\"}");return;
    }
    if (mg_strcmp(hm->uri, mg_str("/api/device/pending")) == 0) { manual_admission_api(c,hm,0);return; }
    if (mg_strcmp(hm->uri, mg_str("/api/device/pending/refresh")) == 0) { manual_admission_refresh_api(c,hm);return; }
    if (mg_strcmp(hm->uri, mg_str("/api/device/authorize")) == 0) { manual_admission_api(c,hm,1);return; }
    //系统设置查询、串口测试与保存
    if (mg_strcmp(hm->uri, mg_str("/api/system/settings")) == 0 &&
        mg_strcmp(hm->method, mg_str("GET")) == 0)
    {
        api_handle_system_settings_get(c);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/system/channel")) == 0 &&
             mg_strcmp(hm->method, mg_str("POST")) == 0)
    {
        api_handle_channel_settings(c, hm);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/system/time")) == 0 &&
             mg_strcmp(hm->method, mg_str("POST")) == 0)
    {
        api_handle_system_time_sync(c, hm);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/system/resources")) == 0 &&
             mg_strcmp(hm->method, mg_str("GET")) == 0)
    {
        api_handle_system_resources_get(c);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/system/serial/devices")) == 0 &&
             mg_strcmp(hm->method, mg_str("GET")) == 0)
    {
        api_handle_serial_devices_get(c);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/system/serial/test")) == 0)
    {
        api_handle_serial_settings(c, hm, 0);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/system/serial")) == 0)
    {
        api_handle_serial_settings(c, hm, 1);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/monitor/policies")) == 0)
    {
        api_handle_monitor_policies(c, hm);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/monitor/policy/delete")) == 0 &&
             mg_strcmp(hm->method, mg_str("POST")) == 0)
    {
        api_handle_monitor_policy_delete(c, hm);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/monitor/settings")) == 0)
    {
        api_handle_monitor_settings(c, hm);
    }
    //添加白名单设备 (POST /api/device/add)
    else if (mg_strcmp(hm->uri, mg_str("/api/device/add")) == 0)
    {
        api_handle_device_add(c, hm);
    } 
    // 拉取设备列表 (GET /api/device/list)
    else if (mg_strcmp(hm->uri, mg_str("/api/device/list")) == 0) 
    {
        api_handle_device_list(c, hm);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/device/sampling")) == 0)
    {
        api_handle_device_sampling(c, hm);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/device/collection_history")) == 0)
    {
        api_handle_collection_history(c, hm);
    }
    // 查询设备温度历史 (GET /api/device/temp_history?mac=XXX&seconds=300)
    else if (mg_strcmp(hm->uri, mg_str("/api/device/temp_history")) == 0)
    {
        api_handle_temp_history(c, hm);
    }
    // 查询设备两路麦克风历史
    else if (mg_strcmp(hm->uri, mg_str("/api/device/sound_history")) == 0)
    {
        api_handle_sound_history(c, hm);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/device/vibration/latest")) == 0 &&
             mg_strcmp(hm->method, mg_str("GET")) == 0)
    {
        api_handle_vibration_latest(c, hm);
    }
    // 移除设备 (POST /api/device/remove)
    else if (mg_strcmp(hm->uri, mg_str("/api/device/remove")) == 0) 
    {
        api_handle_device_remove(c, hm);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/device/description")) == 0)
    {
        api_handle_device_description(c, hm);
    }
    // 修改或恢复节点自定义名称 (POST /api/device/rename)
    else if (mg_strcmp(hm->uri, mg_str("/api/device/rename")) == 0)
    {
        api_handle_device_rename(c, hm);
    }
    else if (mg_strcmp(hm->uri, mg_str("/api/device/scan_signal")) == 0 &&
             mg_strcmp(hm->method, mg_str("POST")) == 0)
    {
        char manual_text[8];
        int manual = mg_http_get_var(&hm->query, "manual", manual_text, sizeof(manual_text)) > 0 &&
                     strcmp(manual_text, "1") == 0;
        pthread_mutex_lock(&g_pending_mutex);
        int active = g_scan_state == 1;
        int busy = signal_refresh_cursor >= 0;
        for (int i = 0; i < g_pending_count; ++i)
            if (!g_pending_list[i].is_confirmed && g_pending_list[i].signal_deadline_ms) busy = 1;
        if (active && !busy) {
            signal_refresh_cursor = 0;
            signal_batch_manual = manual;
        } else if (active && manual) {
            /* A manual click can request logging of the ongoing automatic probe. */
            signal_batch_manual = 1;
            for (int i = 0; i < g_pending_count; ++i)
                if (!g_pending_list[i].is_confirmed && g_pending_list[i].signal_deadline_ms)
                    g_pending_list[i].signal_log_requested = 1;
        }
        pthread_mutex_unlock(&g_pending_mutex);
        mg_http_reply(c, active ? 202 : 409,
            "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
            active ? "{\"status\":\"success\"}" : "{\"status\":\"error\",\"msg\":\"扫描未开启\"}");
    }
    else if (mg_strcmp(hm->uri,mg_str("/api/device/interview"))==0)
        interview_retry_api(c,hm);
    else if (mg_strcmp(hm->uri,mg_str("/api/device/scan_status"))==0)
        join_api(c,hm,-1);
    else if (mg_strcmp(hm->uri,mg_str("/api/device/scan"))==0)
        join_api(c,hm,1);
    else if (mg_strcmp(hm->uri,mg_str("/api/device/scan_stop"))==0)
        join_api(c,hm,0);
    else 
    {
        mg_http_reply(c, 404, "Access-Control-Allow-Origin: *\r\n", "Not Found");
    }
}

/*********************************************************************
 * 独立任务 3：清理任务
 *********************************************************************/
void task_cleanup_pending(void)
{
    /* Candidate cancellation is handled durably by the admission API/tick;
     * approved nodes are never removed by closing PermitJoin. */
}

/*********************************************************************
 * API 业务 2：拉取设备白名单列表 (供前端初始化使用)
 *********************************************************************/
void api_handle_device_list(struct mg_connection *c, struct mg_http_message *hm) 
{
    (void)hm;
    DeviceRecord_t *records = NULL;
    int count = get_confirmed_devices(&records);
    
    cJSON *root = cJSON_CreateArray();
    
    for (int i = 0; i < count; i++) 
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", records[i].mac);
        // name 是最终显示名称；型号与用户自定义名称分别提供给前端
        cJSON_AddStringToObject(item, "name", records[i].name);
        cJSON_AddStringToObject(item, "model_name", records[i].model_name);
        cJSON_AddStringToObject(item, "custom_name", records[i].custom_name);
        cJSON_AddStringToObject(item, "description", records[i].description);
        char *address_end = NULL;
        unsigned long address = strtoul(records[i].short_addr, &address_end, 16);
        if (address_end != records[i].short_addr && *address_end == '\0' && address < 0xfff8UL)
            cJSON_AddNumberToObject(item, "short_addr", (double)address);
        else cJSON_AddNullToObject(item, "short_addr");
        cJSON_AddNumberToObject(item, "last_active", (double)records[i].last_active);
        cJSON_AddStringToObject(item, "status", records[i].status); 
        
        // Device identity/link state and the latest persisted measurements are independent.
        cJSON_AddStringToObject(item, "temp", records[i].temp);
        cJSON_AddStringToObject(item, "mag", records[i].mag); 
        cJSON_AddStringToObject(item, "digital_db", records[i].digital_db);
        cJSON_AddStringToObject(item, "analog_db", records[i].analog_db);
        cJSON_AddStringToObject(item, "work_mode", records[i].work_mode);
        cJSON_AddBoolToObject(item, "online_monitoring_enabled", records[i].monitoring_enabled);
        cJSON_AddStringToObject(item, "collection_state", records[i].collection_state);
        cJSON_AddNumberToObject(item,"collection_batch_id",records[i].collection_batch_id);
        cJSON_AddNumberToObject(item,"capture_batch_id",records[i].capture_batch_id);
        cJSON_AddBoolToObject(item,"capture_pending",records[i].capture_pending);
        cJSON_AddNumberToObject(item,"captured_at",(double)get_collection_capture_time(records[i].mac,records[i].collection_batch_id));
        cJSON_AddNumberToObject(item,"pending_captured_at",(double)get_collection_capture_time(records[i].mac,records[i].capture_batch_id));
        cJSON_AddNumberToObject(item, "last_collection", (double)records[i].last_collection);
        LatestDeviceReadings_t readings;
        if (get_latest_device_readings(records[i].mac, &readings))
        {
            cJSON *latest = cJSON_AddObjectToObject(item, "latest_readings");
            cJSON_AddNumberToObject(latest, "timestamp", (double)readings.timestamp);
            cJSON_AddNumberToObject(latest, "batch_id", readings.batch_id);
            cJSON_AddNumberToObject(latest, "captured_at", (double)get_collection_capture_time(records[i].mac, readings.batch_id));
            if (readings.present_mask & 1U)
                cJSON_AddNumberToObject(latest, "temperature", readings.values[0]);
            if ((readings.present_mask & 0x0EU) == 0x0EU)
            {
                cJSON *magnetic = cJSON_AddObjectToObject(latest, "magnetic");
                cJSON_AddNumberToObject(magnetic, "x", readings.values[1]);
                cJSON_AddNumberToObject(magnetic, "y", readings.values[2]);
                cJSON_AddNumberToObject(magnetic, "z", readings.values[3]);
            }
            if (readings.present_mask & (1U << 4))
                cJSON_AddNumberToObject(latest, "digital_db", readings.values[4]);
            if (readings.present_mask & (1U << 5))
                cJSON_AddNumberToObject(latest, "analog_db", readings.values[5]);
            if (readings.features_ready)
            {
                cJSON_AddNumberToObject(latest, "features_batch_id", readings.features_batch_id);
                cJSON_AddNumberToObject(latest, "features_timestamp", (double)readings.features_timestamp);
                cJSON *features = cJSON_AddArrayToObject(latest, "features");
                for (unsigned axis = 0; axis < 3; axis++)
                {
                    cJSON *row = cJSON_CreateArray();
                    for (unsigned feature = 0; feature < 5; feature++)
                        cJSON_AddItemToArray(row, cJSON_CreateNumber(readings.features[axis][feature] / 1000.0));
                    cJSON_AddItemToArray(features, row);
                }
            }
        }
        cJSON_AddNumberToObject(item, "monitor_policy_id", records[i].monitor_policy_id);
        if (records[i].lqi >= 0)
            cJSON_AddNumberToObject(item, "lqi", records[i].lqi);
        else cJSON_AddNullToObject(item, "lqi");
        if (records[i].rssi != -128)
            cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        else cJSON_AddNullToObject(item, "rssi");
        cJSON_AddNumberToObject(item, "power_source", records[i].power_source);
        cJSON_AddNumberToObject(item, "link_updated_at", (double)records[i].link_updated_at);
        char *details=load_device_interview(records[i].mac);
        cJSON *interview=details?cJSON_Parse(details):NULL; free(details);
        cJSON_AddItemToObject(item,"interview",interview?interview:cJSON_CreateObject());

        cJSON_AddItemToArray(root, item);
    }
    
    if (records) free(records);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n", "%s", json_str);
    free(json_str); 
}

/*********************************************************************
 * API 业务：查询指定指标最近最多30个不同采集批次
 * GET /api/device/collection_history?mac=...&metric=temperature
 *********************************************************************/
void api_handle_collection_history(struct mg_connection *c, struct mg_http_message *hm)
{
    const char *headers = "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n";
    if (mg_strcmp(hm->method, mg_str("GET")) != 0) {
        mg_http_reply(c, 405, headers, "{\"status\":\"error\",\"msg\":\"GET required\"}"); return;
    }
    char mac[32], metric[24];
    if (mg_http_get_var(&hm->query, "mac", mac, sizeof(mac)) <= 0 ||
        mg_http_get_var(&hm->query, "metric", metric, sizeof(metric)) <= 0 ||
        (strcmp(metric,"temperature") && strcmp(metric,"digital") &&
         strcmp(metric,"ultrasonic") && strcmp(metric,"magnetic"))) {
        mg_http_reply(c, 400, headers, "{\"status\":\"error\",\"msg\":\"Invalid mac or metric\"}"); return;
    }
    CollectionHistoryRecord_t *records = NULL;
    int count = get_collection_history(mac, metric, COLLECTION_HISTORY_LIMIT, &records);
    if (count < 0) {
        mg_http_reply(c, 500, headers, "{\"status\":\"error\",\"msg\":\"读取采集历史失败\"}"); return;
    }
    cJSON *root = cJSON_CreateObject(), *data = cJSON_CreateArray();
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddStringToObject(root, "metric", metric);
    cJSON_AddNumberToObject(root, "limit", COLLECTION_HISTORY_LIMIT);
    cJSON_AddNumberToObject(root, "count", count);
    const char *keys[] = {"temperature","mag_x","mag_y","mag_z","digital_db","analog_db"};
    for (int i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "batch_id", records[i].batch_id);
        cJSON_AddNumberToObject(item, "timestamp", (double)records[i].timestamp);
        cJSON_AddNumberToObject(item, "captured_at", (double)records[i].captured_at);
        for (int field = 0; field < 6; ++field) {
            if (records[i].present_mask & (1U << field))
                cJSON_AddNumberToObject(item, keys[field], records[i].values[field]);
            else cJSON_AddNullToObject(item, keys[field]);
        }
        cJSON_AddItemToArray(data, item);
    }
    free(records);
    cJSON_AddItemToObject(root, "data", data);
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    mg_http_reply(c, json ? 200 : 500, headers, "%s", json ? json : "{\"status\":\"error\"}");
    free(json);
}

/*********************************************************************
 * 查询设备温度历史（保留原有按时间范围查询的接口）
 * GET /api/device/temp_history?mac=...&seconds=300
 *********************************************************************/
void api_handle_temp_history(struct mg_connection *c, struct mg_http_message *hm)
{
    char mac[32] = {0};
    char seconds_buf[16] = {0};

    /*
     * 从 URL 查询参数中提取 mac。
     */
    int mac_len = mg_http_get_var(&hm->query,"mac",mac,sizeof(mac));

    if (mac_len <= 0)
    {
        mg_http_reply(
            c,
            400,
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n",
            "{\"status\":\"error\",\"msg\":\"Missing mac\"}"
        );

        return;
    }

    /*
     * 默认查询最近5分钟。
     *
     * 前端如果传：
     * seconds=600
     * 就查询最近10分钟。
     */
    int history_seconds = 300;

    int seconds_len = mg_http_get_var(
        &hm->query,
        "seconds",
        seconds_buf,
        sizeof(seconds_buf)
    );

    if (seconds_len > 0)
    {
        int requested_seconds = atoi(seconds_buf);

        if (requested_seconds > 0)
        {
            history_seconds = requested_seconds;
        }
    }

    /*
     * 数据库最多只保留12小时，
     * 所以这里也限制查询时间最大不超过12小时。
     */
    if (history_seconds > SENSOR_HISTORY_RETENTION_SEC)
    {
        history_seconds = SENSOR_HISTORY_RETENTION_SEC;
    }


    TemperatureRecord_t *records = NULL;

    int count = get_temperature_history(
        mac,
        history_seconds,
        &records
    );


    /*
     * 组装返回 JSON。
     */
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();

    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "seconds", history_seconds);
    cJSON_AddNumberToObject(root, "count", count);


    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();

        cJSON_AddNumberToObject(
            item,
            "temperature",
            records[i].temperature
        );

        cJSON_AddNumberToObject(
            item,
            "timestamp",
            (double)records[i].timestamp
        );

        cJSON_AddItemToArray(data, item);
    }


    cJSON_AddItemToObject(root, "data", data);

    if (records)
    {
        free(records);
    }


    char *json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);


    mg_http_reply(
        c,
        200,
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n",
        "%s",
        json_str
    );

    free(json_str);
}

/*********************************************************************
 * API 业务：查询设备数字和超声麦克风历史
 * GET /api/device/sound_history?mac=XX:XX:XX:XX:XX:XX:XX:XX&seconds=1800
 *********************************************************************/
void api_handle_sound_history(struct mg_connection *c, struct mg_http_message *hm)
{
    char mac[32] = {0};
    char seconds_buf[16] = {0};

    if (mg_http_get_var(&hm->query, "mac", mac, sizeof(mac)) <= 0)
    {
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Missing mac\"}");
        return;
    }

    int history_seconds = 300;
    if (mg_http_get_var(&hm->query, "seconds", seconds_buf, sizeof(seconds_buf)) > 0)
    {
        int requested_seconds = atoi(seconds_buf);
        if (requested_seconds > 0)
        {
            history_seconds = requested_seconds;
        }
    }

    if (history_seconds > SENSOR_HISTORY_RETENTION_SEC)
    {
        history_seconds = SENSOR_HISTORY_RETENTION_SEC;
    }

    SoundRecord_t *records = NULL;
    int count = get_sound_history(mac, history_seconds, &records);
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();

    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "seconds", history_seconds);
    cJSON_AddNumberToObject(root, "count", count);

    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "source",
                                records[i].is_digital ? "digital" : "analog");
        cJSON_AddNumberToObject(item, "db_spl", records[i].db_spl);
        cJSON_AddNumberToObject(item, "timestamp", (double)records[i].timestamp);
        cJSON_AddItemToArray(data, item);
    }

    cJSON_AddItemToObject(root, "data", data);
    free(records);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    mg_http_reply(c, 200,
                  "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "%s", json_str);
    free(json_str);
}

/*********************************************************************
 * 业务处理函数：处理 0x86 (设备主动上报温度心跳)
 *********************************************************************/
void handle_cmd_86_temperature(unsigned char *payload, unsigned char len) 
{
    if (len < 8) return; // 短地址(2) + 采集批次(4) + 温度(2)
    
    // 1. 提取短地址 (小端模式：低位在前，高位在后)
    uint16_t short_addr = payload[0] | (payload[1] << 8);
    
    // 2. 提取温度原始数值 (小端模式)
    uint32_t collection_id = read_le32(payload + 2);
    int16_t raw_temp = (int16_t)read_le16(payload + 6);
    
    // 3. 换算为真实摄氏度 (Zigbee 发过来时放大了 100 倍)
    float temp_c = raw_temp / 100.0f;
    
    // 4. 将短地址转为字符串，去数据库里反查出 MAC 地址
    char short_addr_str[16];
    snprintf(short_addr_str, sizeof(short_addr_str), "%04X", short_addr);
    char mac_str[24] = {0};
    
    // 💡 注意：这里调用了底层的查表函数，因为前端只会认 MAC 地址
    if (get_mac_by_short_addr(short_addr_str, mac_str) == 0) 
    {
        // 🚀 嫌疑人三被击毙：找不到 MAC 时大声报错，不再静默死掉！
        printf("⚠️ [幽灵数据] 收到短地址 0x%04X 的温度 %.2f ℃，但数据库里查不到它！\n", short_addr, temp_c);
        // 如果查不到对应的设备（可能是被踢出的幽灵节点），直接忽略
        return; 
    }

    // 每次有效温度心跳都刷新在线状态，避免设备持续上报却被看门狗误判离线
    update_device_online(mac_str, short_addr_str);
    int recorded=record_collection_temp(mac_str,collection_id,temp_c);
    if(recorded<0)fprintf(stderr,"[数据入库失败] 温度 MAC=%s | 批次=%u\n",mac_str,collection_id);
    if(recorded<=0)return;
    printf("🌡️ [温度] MAC=%s | 批次=%u | %.2f ℃\n", mac_str, collection_id, temp_c);

    // 5. 组装特定 type 的 JSON 并跨线程投递给 WebSocket 广播
    char json_buf[128];
    snprintf(json_buf, sizeof(json_buf), 
             "{\"type\":\"temp_update\",\"mac\":\"%s\",\"temp\":\"%.2f\",\"batch_id\":%u}",
             mac_str, temp_c, collection_id);

    pthread_mutex_lock(&g_ws_mutex);
    strcpy(g_ws_msg_buffer, json_buf);
    queue_ws_event_locked();
    pthread_mutex_unlock(&g_ws_mutex);
}

/*********************************************************************
 * 业务处理函数：处理 0x87 (设备主动上报磁力计三轴数据)
 *********************************************************************/
void handle_cmd_87_magnetic(unsigned char *payload, unsigned char len) 
{
    if (len < 12) return; // 短地址(2) + 采集批次(4) + 三轴原始数据(6)
    
    // 1. 提取短地址 (小端模式)
    uint16_t short_addr = payload[0] | (payload[1] << 8);
    
    // 2. 提取三轴原始 int16_t 数值
    uint32_t collection_id = read_le32(payload + 2);
    int16_t raw_x = (int16_t)read_le16(payload + 6);
    int16_t raw_y = (int16_t)read_le16(payload + 8);
    int16_t raw_z = (int16_t)read_le16(payload + 10);
    
    // 3. 按照 IIS2MDC 芯片手册换算为微特斯拉 (uT)，每个 LSB 代表 0.15 uT
    float x_uT = raw_x * 0.15f;
    float y_uT = raw_y * 0.15f;
    float z_uT = raw_z * 0.15f;
    
    // 4. 将短地址转为字符串，去数据库里反查出 MAC 地址
    char short_addr_str[16];
    snprintf(short_addr_str, sizeof(short_addr_str), "%04X", short_addr);
    char mac_str[24] = {0};
    
    if (get_mac_by_short_addr(short_addr_str, mac_str) == 0) 
    {
        printf("⚠️ [幽灵磁力] 收到短地址 0x%04X 的磁力数据，但数据库里查不到它！\n", short_addr);
        return; 
    }

    update_device_online(mac_str,short_addr_str);
    int recorded=record_collection_mag(mac_str,collection_id,x_uT,y_uT,z_uT);
    if(recorded<0)fprintf(stderr,"[数据入库失败] 磁场 MAC=%s | 批次=%u\n",mac_str,collection_id);
    if(recorded<=0)return;
    printf("🧲 [磁场] MAC=%s | 批次=%u | X=%.1f | Y=%.1f | Z=%.1f uT\n",
           mac_str, collection_id, x_uT, y_uT, z_uT);

    // 5. 组装特定 type 的 JSON 并通过 WebSocket 广播给前端
    char json_buf[128];
    snprintf(json_buf, sizeof(json_buf), 
             "{\"type\":\"mag_update\",\"mac\":\"%s\",\"mag\":\"X:%.1f, Y:%.1f, Z:%.1f\",\"batch_id\":%u}",
             mac_str, x_uT, y_uT, z_uT, collection_id);

    pthread_mutex_lock(&g_ws_mutex);
    strcpy(g_ws_msg_buffer, json_buf);
    queue_ws_event_locked();
    pthread_mutex_unlock(&g_ws_mutex);
}

/*********************************************************************
 * 业务处理函数：处理0x88/0x89（一秒数字/超声麦克风声级）
 *********************************************************************/
void handle_cmd_88_89_sound(unsigned char *payload, unsigned char len, int is_digital)
{
    if (len < 8) return; // 短地址2字节 + 采集批次4字节 + 有符号声级2字节

    uint16_t short_addr = (uint16_t)(payload[0] | (payload[1] << 8));
    uint32_t collection_id = read_le32(payload + 2);
    int16_t raw_db = (int16_t)read_le16(payload + 6);
    float db_spl = raw_db / 100.0f;
    char short_addr_str[16];
    char mac_str[24] = {0};

    snprintf(short_addr_str, sizeof(short_addr_str), "%04X", short_addr);
    if (get_mac_by_short_addr(short_addr_str, mac_str) == 0)
    {
        printf("⚠️ [幽灵声级] 收到短地址 0x%04X 的%s麦克风数据，但数据库里查不到它！\n",
               short_addr, is_digital ? "数字" : "超声");
        return;
    }

    update_device_online(mac_str, short_addr_str);
    int recorded=record_collection_sound(mac_str,collection_id,is_digital,db_spl);
    if(recorded<0)fprintf(stderr,"[数据入库失败] 麦克风 MAC=%s | 批次=%u\n",mac_str,collection_id);
    if(recorded<=0)return;
    printf("🎤 [%s麦克风] MAC=%s | 批次=%u | %.2f dB SPL\n",
           is_digital ? "数字" : "超声", mac_str, collection_id, db_spl);

    char json_buf[160];
    snprintf(json_buf, sizeof(json_buf),
             "{\"type\":\"sound_update\",\"mac\":\"%s\",\"source\":\"%s\",\"db\":\"%.2f\",\"timestamp\":%lld,\"batch_id\":%u}",
             mac_str, is_digital ? "digital" : "analog", db_spl,
             (long long)time(NULL), collection_id);

    pthread_mutex_lock(&g_ws_mutex);
    strcpy(g_ws_msg_buffer, json_buf);
    queue_ws_event_locked();
    pthread_mutex_unlock(&g_ws_mutex);
}

/*********************************************************************
 * API 业务：修改节点自定义名称；空名称表示恢复 MAC 短标识默认名称
 *********************************************************************/
static void api_handle_device_description(struct mg_connection *c, struct mg_http_message *hm)
{
    const char *headers = "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n";
    if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
        mg_http_reply(c, 405, headers, "{\"status\":\"error\",\"msg\":\"POST required\"}");
        return;
    }
    cJSON *root = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    cJSON *mac = cJSON_GetObjectItemCaseSensitive(root, "mac");
    cJSON *description = cJSON_GetObjectItemCaseSensitive(root, "description");
    unsigned char mac_bin[8];
    if (!cJSON_IsString(mac) || !cJSON_IsString(description) ||
        mac_string_to_bytes(mac->valuestring, mac_bin) != 0 || strlen(description->valuestring) > 768) {
        cJSON_Delete(root);
        mg_http_reply(c, 400, headers, "{\"status\":\"error\",\"msg\":\"Invalid MAC or description (maximum 768 UTF-8 bytes)\"}");
        return;
    }
    int rc = update_device_description(mac->valuestring, description->valuestring);
    if (rc != 0) {
        cJSON_Delete(root);
        mg_http_reply(c, rc == 2 ? 404 : 500, headers, "{\"status\":\"error\",\"msg\":\"Device not found or database update failed\"}");
        return;
    }
    cJSON *response = cJSON_CreateObject();
    gateway_log_event("INFO", "frontend", "description_changed", mac->valuestring, -1, -1, "用户已%s节点描述", description->valuestring[0] ? "修改" : "恢复默认");
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "mac", mac->valuestring);
    cJSON_AddStringToObject(response, "description", description->valuestring);
    char *json = cJSON_PrintUnformatted(response);
    mg_http_reply(c, 200, headers, "%s", json ? json : "{\"status\":\"error\"}");
    free(json);
    cJSON_AddStringToObject(response, "type", "description_update");
    json = cJSON_PrintUnformatted(response);
    if (json) {
        pthread_mutex_lock(&g_ws_mutex);
        snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer), "%s", json);
        queue_ws_event_locked();
        pthread_mutex_unlock(&g_ws_mutex);
        free(json);
    }
    cJSON_Delete(response);
    cJSON_Delete(root);
}

void api_handle_device_rename(struct mg_connection *c, struct mg_http_message *hm)
{
    char *json_str = (char *)malloc(hm->body.len + 1);
    if (!json_str)
    {
        mg_http_reply(c, 500,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Out of memory\"}");
        return;
    }

    memcpy(json_str, hm->body.buf, hm->body.len);
    json_str[hm->body.len] = '\0';

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (!root)
    {
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Invalid JSON\"}");
        return;
    }

    cJSON *mac = cJSON_GetObjectItem(root, "mac");
    cJSON *custom_name_item = cJSON_GetObjectItem(root, "custom_name");

    if (!cJSON_IsString(mac) || !cJSON_IsString(custom_name_item))
    {
        cJSON_Delete(root);
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Missing mac or custom_name\"}");
        return;
    }

    unsigned char mac_bin[8];
    if (mac_string_to_bytes(mac->valuestring, mac_bin) != 0)
    {
        cJSON_Delete(root);
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"MAC invalid\"}");
        return;
    }

    char normalized_name[128] = {0};
    if (normalize_custom_name(custom_name_item->valuestring,
                              normalized_name,
                              sizeof(normalized_name)) != 0)
    {
        cJSON_Delete(root);
        mg_http_reply(c, 400,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Name must be 0-32 characters without control characters\"}");
        return;
    }

    int update_result = update_device_custom_name(mac->valuestring, normalized_name);
    if (update_result == 1)
    {
        cJSON_Delete(root);
        mg_http_reply(c, 409,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"duplicate\",\"msg\":\"Custom name already exists\"}");
        return;
    }

    if (update_result == 2)
    {
        cJSON_Delete(root);
        mg_http_reply(c, 404,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Device not found\"}");
        return;
    }

    if (update_result != 0)
    {
        cJSON_Delete(root);
        mg_http_reply(c, 500,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Database update failed\"}");
        return;
    }

    char model_name[64] = {0};
    char custom_name[128] = {0};
    char display_name[128] = {0};

    if (!get_device_name_info(mac->valuestring,
                              model_name,
                              sizeof(model_name),
                              custom_name,
                              sizeof(custom_name),
                              display_name,
                              sizeof(display_name)))
    {
        cJSON_Delete(root);
        mg_http_reply(c, 500,
                      "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                      "{\"status\":\"error\",\"msg\":\"Unable to load updated device\"}");
        return;
    }

    publish_name_update(mac->valuestring, model_name, custom_name, display_name);
    gateway_log_event("INFO", "frontend", "name_changed", mac->valuestring, -1, -1, "用户修改节点名称为：%s", display_name);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "name", display_name);
    cJSON_AddStringToObject(response, "model_name", model_name);
    cJSON_AddStringToObject(response, "custom_name", custom_name);
    char *response_json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    cJSON_Delete(root);

    mg_http_reply(c, 200,
                  "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                  "%s", response_json ? response_json : "{\"status\":\"success\"}");

    if (response_json) free(response_json);
}

/*********************************************************************
 * API 业务 3：处理移除设备 (删底库 + 踢出 Zigbee 网络)
 *********************************************************************/
void api_handle_device_remove(struct mg_connection *c, struct mg_http_message *hm) 
{
    char *json_str = (char *)malloc(hm->body.len + 1);
    if (json_str == NULL) 
    {
        mg_http_reply(c, 500, "Access-Control-Allow-Origin: *\r\n", "{\"status\":\"error\",\"msg\":\"Out of memory\"}");
        return;
    }
    memcpy(json_str, hm->body.buf, hm->body.len);
    json_str[hm->body.len] = '\0';
    
    cJSON *root = cJSON_Parse(json_str);
    if (root) 
    {
        cJSON *mac = cJSON_GetObjectItem(root, "mac");
        if (cJSON_IsString(mac)) 
        {
            unsigned char mac_bin[8];
            if (mac_string_to_bytes(mac->valuestring, mac_bin) == 0) 
            {
                uint16_t removed_short = 0;
                int have_short = get_device_short_addr(mac->valuestring,
                                                       &removed_short);

                /* 在同一SQLite事务中先建立撤销保护，再删除业务数据。
                 * 这样任何并发上行都不会落入“既不授权也不撤销”的空窗。 */
                pthread_mutex_lock(&g_vibration_mutex);
                int deleted = revoke_device_and_delete_data(
                    mac->valuestring, have_short ? (int)removed_short : -1);
                if (deleted==0)
                {
                    v2_gateway_remove(mac->valuestring);
                    if (have_short)
                        for (unsigned i=0;i<VIBRATION_ASSEMBLY_SLOTS;++i)
                            if (g_vibration_assemblies[i].active && g_vibration_assemblies[i].short_addr==removed_short)
                                reset_vibration_assembly(&g_vibration_assemblies[i]);
                }
                pthread_mutex_unlock(&g_vibration_mutex);
                if (deleted == 0)
                {
                    channel_migration_remove_node(mac->valuestring);

                    /* Initial submission must not throttle the first fresh uplink. */
                    int submitted = submit_removed_device_leave(mac_bin);
                    printf("🔫 [设备移除] MAC=%s | 业务数据已删除 | %s\n",
                           mac->valuestring, submitted ?
                           "已提交离网请求，未确认时在节点活动后补发" : "等待节点活动后补发离网请求");
                    gateway_log_event("INFO", "frontend", "removal_pending", mac->valuestring, have_short ? removed_short : -1, -1,
                                      "节点业务数据已删除，离网任务已保存；%s",
                                      submitted ? "已提交离网请求，未确认时在节点活动后补发" : "等待节点活动后补发离网请求");
                    mg_http_reply(c, 200,
                                  "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                                  "{\"status\":\"success\",\"msg\":\"Device and related data removed\"}");
                }
                else
                {
                    mg_http_reply(c, 500,
                                  "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
                                  "{\"status\":\"error\",\"msg\":\"Failed to remove device data\"}");
                }
            } 
            else 
            {
                mg_http_reply(c, 400, "Access-Control-Allow-Origin: *\r\n", "{\"status\":\"error\",\"msg\":\"MAC invalid\"}");
            }
        } 
        else 
        {
            mg_http_reply(c, 400, "Access-Control-Allow-Origin: *\r\n", "{\"status\":\"error\",\"msg\":\"Missing 'mac' field\"}");
        }
        cJSON_Delete(root);
    } 
    else 
    {
        mg_http_reply(c, 400, "Access-Control-Allow-Origin: *\r\n", "{\"status\":\"error\",\"msg\":\"Invalid JSON\"}");
    }
    free(json_str);
}

/*********************************************************************
 * 独立任务 4：周期维护。
 * 明确的离网事件立即生效；突然断电则按节点最后实际获得的采集周期，连续
 * 错过两轮后判离线。每轮只处理一个节点，避免单槽WebSocket缓冲被覆盖。
 *********************************************************************/
void task_periodic_maintenance(void)
{
    static unsigned long last_history_cleanup_time = 0;
    static unsigned long last_online_check_time = 0;
    static uint64_t last_coordinator_query_time = 0U;
    unsigned long current_time = (unsigned long)mg_millis(); 

    if ((uint64_t)current_time - last_coordinator_query_time >=
        COORDINATOR_QUERY_PERIOD_MS)
    {
        request_network_info();
        last_coordinator_query_time = (uint64_t)current_time;
    }

    if (current_time - last_online_check_time >= ONLINE_STATUS_CHECK_PERIOD)
    {
        time_t now = time(NULL);
        if ((long long)now >= MIN_VALID_SYSTEM_EPOCH)
        {
            DeviceRecord_t *records = NULL;
            int count = get_all_devices(&records);
            for (int i = 0; i < count; ++i)
            {
                uint64_t interval = records[i].expected_interval_sec != 0U
                    ? records[i].expected_interval_sec
                    : MONITOR_DEFAULT_AUTO_INTERVAL_SEC;
                uint64_t offline_after = interval * 2ULL + ONLINE_STATUS_GRACE_SEC;
                if (strcasecmp(records[i].status, "online") == 0 &&
                    records[i].last_active > 0 &&
                    (long long)now > records[i].last_active &&
                    (uint64_t)((long long)now - records[i].last_active) > offline_after)
                {
                    if (update_device_offline(records[i].mac) == 0)
                    {
                        printf("⚫ [在线超时] 节点 %s 连续错过两轮采集，%llu 秒无上行，已标记离线。\n",
                               records[i].mac,
                               (unsigned long long)((long long)now - records[i].last_active));
                        gateway_log_event("WARN", "backend", "node_offline", records[i].mac, -1, -1,
                                          "连续错过两轮采集，%lld 秒无上行，标记离线", (long long)now - records[i].last_active);
                        pthread_mutex_lock(&g_ws_mutex);
                        snprintf(g_ws_msg_buffer, sizeof(g_ws_msg_buffer),
                                 "{\"type\":\"status_update\",\"mac\":\"%s\",\"status\":\"offline\"}",
                                 records[i].mac);
                        queue_ws_event_locked();
                        pthread_mutex_unlock(&g_ws_mutex);
                    }
                    break;
                }
            }
            free(records);
        }
        last_online_check_time = current_time;
    }

    // 每60秒清理一次超过12小时的温度、磁场和麦克风历史数据
    if (current_time - last_history_cleanup_time >= HISTORY_CLEANUP_PERIOD)
    {
        int deleted_count = cleanup_sensor_history(SENSOR_HISTORY_RETENTION_SEC);

        if (deleted_count > 0)
        {
            printf("🧹 [历史清理] 已删除 %d 条超过12小时的传感器历史数据。\n", deleted_count);
        }
        else if (deleted_count < 0)
        {
            printf("❌ [历史清理] 清理历史数据失败。\n");
        }

        last_history_cleanup_time = current_time;
    }        
}
