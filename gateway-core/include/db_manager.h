#ifndef DB_MANAGER_H
#define DB_MANAGER_H

#include <stddef.h>
#include <stdint.h>

// 💡 升级后的设备结构体：增加了用于缓存最新传感器数据的字符串缓冲区
typedef struct {
    char name[128];      // 前端实际显示名称（自定义名称优先，否则使用默认名称）
    char model_name[64]; // Zigbee Basic Cluster 上报的产品型号
    char custom_name[128]; // 用户自定义名称；空字符串表示使用默认名称
    char mac[32];        // MAC 地址
    char short_addr[16]; // 短地址 (作为字符串存储)
    char description[769]; // 用户描述，最多 768 个 UTF-8 字节
    char status[16];     // 状态："offline", "waiting", "online"
    
    // 👇 新增：用于前端刷新时秒开显示的最新数据缓存
    char temp[16];       // 最新温度字符串 (如 "26.50" 或 "--")
    char mag[64];        // 最新磁力字符串 (如 "X:1.2, Y:3.4, Z:5.6" 或 "--")
    char digital_db[16]; // 最新数字麦克风声级字符串
    char analog_db[16];  // 最新超声麦克风声级字符串
    char work_mode[16];  // 工作模式（睡眠还是.....）
    int monitoring_enabled; // 自动在线监测开关；新设备默认关闭
    char collection_state[16]; // idle/waiting/capturing/uploading/complete/error
    long long last_collection; // 最近一次完整采集时间
    long long last_active; // 最近一次有效上行时间
    uint32_t expected_interval_sec; // 最近一次实际下发给该节点的采集周期
    int monitor_policy_id;
    uint32_t raw_upload_generation;
    int lqi; // -1 means no received measurement
    int rssi; // -128 means unknown
    int power_source; // Zigbee Basic power source: 0 unknown, 4 external DC
    long long link_updated_at; // measurement time, not an online heartbeat
    uint32_t collection_batch_id, capture_batch_id;
    int capture_pending;
    int join_confirmed; // Persisted application uplink proof, never Device Announce alone.
    int user_authorized; // Explicit user admission; distinct from wireless readiness.
    int sampling_admission; // 0 legacy, 1 awaiting node confirmation, 2 local defaults accepted.
} DeviceRecord_t;

int device_is_authorized(const char *mac);
int device_admission_ready(const char *mac);
int request_device_admission(const char *mac);
int device_admission_requested(const char *mac);
int authorize_device(const char *mac); /* 1 new grant, 0 already granted, -2 not ready, -1 DB error */

int update_device_link_telemetry(const char *mac, int lqi, int rssi, int power_source);
/* Active probe result: preserve independently discovered power-source metadata. */
int update_pending_probe_telemetry(const char *mac, int lqi, int rssi);

typedef struct
{
    int id;
    char name[128];
    int is_default;
    uint32_t auto_interval_sec;
    uint16_t poll_interval_sec;
    uint16_t vibration_rate_hz;
    uint16_t digital_mic_rate_hz;
    uint32_t analog_mic_rate_hz;
    uint16_t vibration_window_ms;
    uint16_t digital_mic_window_ms;
    uint16_t analog_mic_window_ms;
    uint16_t raw_upload_every_cycles;
    uint16_t post_upload_rx_window_sec;
    uint8_t sensor_enable_mask;
} MonitorPolicyRecord_t;

int save_device_monitor_policy(const char *mac, MonitorPolicyRecord_t *policy,
                               int *out_policy_id);


typedef struct
{
    float temperature;
    long long timestamp;
} TemperatureRecord_t;

typedef struct
{
    float db_spl;
    int is_digital;
    long long timestamp;
} SoundRecord_t;

typedef struct
{
    char state[16];
    int strategy;
    int target_channel;
    int current_channel;
    int coordinator_status;
    int waiting_for_nodes;
    int commit_sent;
    int node_count;
    int prepared_count;
    char message[160];
} ChannelOperationRecord_t;

typedef struct
{
    char mac[32];
    uint16_t short_addr;
    uint8_t state;
} ChannelOperationNodeRecord_t;

typedef struct
{
    char mac[32];
    uint16_t last_short_addr;
    int has_short_addr;
    char state[16]; /* pending / sending / guard */
    long long deleted_at;
    long long last_seen_at;
    long long last_leave_at;
    unsigned int retry_count;
} RevokedNodeRecord_t;

// 数据库操作接口
int init_database(void);

typedef struct {
    uint32_t mean_unit_ms, samples, last_duration_ms, last_blocks, last_batch_id;
} UploadTimingRecord_t;
/* Per-MAC timing, separate for waveform (per window) and features (per batch).
 * These are historical statistics, never persisted queue entries or leases.
 * get: 1 found, 0 unknown, -1 SQL/error. save: 1 learned, 0 duplicate, -1 error. */
int get_upload_timing(const char *mac,unsigned raw_required,UploadTimingRecord_t *out);
int save_upload_timing(const char *mac,uint32_t batch,unsigned raw_required,
                       unsigned blocks,uint64_t elapsed_ms,UploadTimingRecord_t *out);
typedef struct {
    int id;
    char name[129];
    char description[769];
} NodeGroupRecord_t;
int get_node_groups(NodeGroupRecord_t **records);
int get_node_group_members(int id, char (**macs)[32]);
/* 0 success, 1 member already grouped, 2 invalid/missing record, -1 DB error. */
int save_node_group(int *id, const char *name, const char *description,
                    const char *const *macs, int count);
int delete_node_group(int id);
/* 1 applied configuration exists, 0 never applied, -1 database error. */
int get_node_group_sampling(int id, MonitorPolicyRecord_t *policy);
int apply_node_group_sampling(int id, const MonitorPolicyRecord_t *policy, int *updated);
void close_database(void);
int is_device_in_db(const char *mac);
/* Log classification only: a previously confirmed node was offline or changed
 * address. A duplicate announcement of a newly discovered node is not a reconnect. */
int device_announcement_is_reconnect(const char *mac, uint16_t short_addr);

// 状态管理函数
int insert_new_device(const char *mac, const char *model_name);
int save_device_interview(const char *mac, const char *json);
char *load_device_interview(const char *mac); /* caller frees */
int update_device_online(const char *mac, const char *short_addr);
int update_device_offline(const char *mac);
int update_device_expected_interval(const char *mac, uint32_t interval_sec);
int delete_device(const char *mac);
int revoke_device_and_delete_data(const char *mac, int short_addr);
int cancel_unapproved_device(const char *mac, int short_addr); /* 0 revoked, 1 authorized/missing, -1 DB error */
int queue_revoked_node(const char *mac, uint16_t short_addr);
int get_revoked_node_by_mac(const char *mac, RevokedNodeRecord_t *record);
int get_revoked_node_by_short(uint16_t short_addr, RevokedNodeRecord_t *record);
int update_revoked_node_seen(const char *mac, uint16_t short_addr,
                             const char *state);
int mark_revoked_leave_attempt(const char *mac, uint16_t short_addr);
int mark_revoked_node_guard(const char *mac, uint16_t short_addr);
int clear_revoked_node(const char *mac);
int release_revoked_short_for_other_mac(const char *mac,
                                        uint16_t short_addr);
int update_device_model_name(const char *mac, const char *model_name);
int update_device_custom_name(const char *mac, const char *custom_name);
int update_device_description(const char *mac, const char *description);
int get_device_name_info(const char *mac,
                         char *model_name,
                         size_t model_name_size,
                         char *custom_name,
                         size_t custom_name_size,
                         char *display_name,
                         size_t display_name_size);
void build_default_device_name(const char *model_name,
                               const char *mac,
                               char *out_name,
                               size_t out_name_size);

// 💡 新增：向独立流水表中插入传感器数据的 API
int insert_sensor_temp(const char *mac, float temperature);
int insert_sensor_mag(const char *mac, float mag_x, float mag_y, float mag_z);
int insert_sensor_sound(const char *mac, int is_digital, float db_spl);
int upsert_collection_temp(const char *mac, uint32_t collection_id, float value);
int upsert_collection_mag(const char *mac, uint32_t collection_id,
                          float x, float y, float z);
int upsert_collection_sound(const char *mac, uint32_t collection_id,
                            int is_digital, float value);
/* Atomically update per-batch values, device shadow and scalar history.
 * 1 new/changed data, 0 identical replay, -1 error (retry remains possible). */
int record_collection_temp(const char *mac, uint32_t batch, float value);
int record_collection_mag(const char *mac, uint32_t batch, float x, float y, float z);
int record_collection_sound(const char *mac, uint32_t batch, int is_digital, float value);
int get_collection_values(const char *mac, uint32_t collection_id,
                          float *temperature, float *mag_x, float *mag_y,
                          float *mag_z, float *digital_db, float *analog_db,
                          unsigned int *present_mask);
int update_device_monitoring(const char *mac, int enabled);
int get_device_monitoring(const char *mac, int *enabled);
int update_device_collection_state(const char *mac, const char *state, int mark_completed);
int update_device_collection_batch_state(const char *mac,const char *state,int mark_completed,uint32_t batch);
/* First gateway observation of this batch's capture-start notification, seconds
 * since Unix epoch. Zero means unknown; never substitute upload receipt time. */
long long get_collection_capture_time(const char *mac, uint32_t batch);
/* Predict the next strictly future capture on this MAC's last capture grid.
 * Unknown history uses a persistent fallback anchor. 1=plan, 0=invalid, -1=DB error. */
int get_sampling_resume_plan(const char *mac, uint32_t period_sec, long long now_ms,
                             uint32_t *deadline_sec, uint32_t *delay_ms);
int get_device_short_addr(const char *mac, uint16_t *out_short_addr);

int save_monitor_config(uint32_t auto_interval_sec,
                        uint16_t poll_interval_sec,
                        uint16_t vibration_rate_hz,
                        uint16_t digital_mic_rate_hz,
                        uint32_t analog_mic_rate_hz,
                        uint16_t vibration_window_ms,
                        uint16_t digital_mic_window_ms,
                        uint16_t analog_mic_window_ms,
                        uint16_t post_upload_rx_window_sec);
int load_monitor_config(uint32_t *auto_interval_sec,
                        uint16_t *poll_interval_sec,
                        uint16_t *vibration_rate_hz,
                        uint16_t *digital_mic_rate_hz,
                        uint32_t *analog_mic_rate_hz,
                        uint16_t *vibration_window_ms,
                        uint16_t *digital_mic_window_ms,
                        uint16_t *analog_mic_window_ms,
                        uint16_t *post_upload_rx_window_sec);
int get_monitor_policies(MonitorPolicyRecord_t **out_records);
int save_monitor_policy(MonitorPolicyRecord_t *policy,
                        const char *const *node_macs,
                        int node_count,
                        int *out_policy_id);
int delete_monitor_policy(int policy_id);
int load_device_monitor_policy(const char *mac,
                               MonitorPolicyRecord_t *policy,
                               uint32_t *raw_upload_generation);
int save_zigbee_channel_config(const char *strategy, int target_channel);
int load_zigbee_channel_config(char *strategy, size_t strategy_size,
                               int *target_channel);
int reset_zigbee_channel_config(void);
int save_channel_operation_record(const ChannelOperationRecord_t *record);
int save_channel_operation_snapshot(const ChannelOperationRecord_t *record,
                                    const ChannelOperationNodeRecord_t *nodes,
                                    int count,
                                    char *error_message,
                                    size_t error_message_size);
int load_channel_operation_record(ChannelOperationRecord_t *record);
int replace_channel_operation_nodes(const ChannelOperationNodeRecord_t *nodes,
                                    int count);
int load_channel_operation_nodes(ChannelOperationNodeRecord_t **nodes,
                                 int *count);
int update_channel_operation_node(const char *mac, uint16_t short_addr,
                                  uint8_t state);
int delete_channel_operation_node(const char *mac);
int save_zigbee_network_state(uint16_t pan_id, uint8_t actual_channel,
                              const char *startup_source);
int clear_zigbee_network_state(void);
int load_zigbee_network_state(uint16_t *pan_id, uint8_t *actual_channel,
                              char *startup_source, size_t startup_source_size);
int save_vibration_batch(const char *mac,
                         uint32_t batch_id,
                         uint32_t sample_rate_hz,
                         uint32_t sample_count,
                         const void *samples,
                         size_t sample_bytes,
                         const int32_t features[3][5]);
int get_latest_vibration_batch(const char *mac,
                               uint32_t *batch_id,
                               uint32_t *sample_rate_hz,
                               uint32_t *sample_count,
                               void **samples,
                               size_t *sample_bytes,
                               int32_t features[3][5],
                               long long *timestamp);
/* requested_batch=0 selects latest; completed_only requires durable raw + features. */
int get_vibration_batch(const char *mac, uint32_t requested_batch, int completed_only,
                       uint32_t *batch_id, uint32_t *sample_rate_hz, uint32_t *sample_count,
                       void **samples, size_t *sample_bytes, int32_t features[3][5], long long *timestamp);
/* Last valid values per sensor and last received vibration features. These
 * survive capture/upload state changes and do not imply the node is online. */
typedef struct {
    float values[6]; /* temperature, magnetic XYZ, digital dB, analog dB */
    unsigned int present_mask;
    long long timestamp;
    int features_ready;
    uint32_t batch_id;
    uint32_t features_batch_id;
    long long features_timestamp;
    int32_t features[3][5];
} LatestDeviceReadings_t;
int get_latest_device_readings(const char *mac, LatestDeviceReadings_t *readings);
/* Exact batch verification for idempotent V2 final ACK after gateway restart.
 * 1 complete+identical, 0 missing/incomplete, -1 conflict/database error. */
int vibration_batch_matches(const char *mac, const uint8_t descriptor[77]);
/* 1 exact durable summary, 0 absent/different, -1 database error. */
int vibration_summary_matches(const char *mac, uint32_t batch, uint32_t rate,
                               uint32_t count, const int32_t features[3][5]);
/* Durable evidence for replaying a completed upload, independent of MQTT.
 * Identity: first 17 descriptor bytes; manifest: the verified scalar snapshot. */
typedef struct {
    uint8_t identity[17], manifest[24];
    unsigned raw_required;
} UploadBatchReceipt_t;
int remember_upload_batch(const char *mac, const uint8_t descriptor[77], unsigned raw_required);
int remember_upload_manifest(const char *mac, uint32_t batch, const uint8_t manifest[24]);
/* 1 raw (when required) + all features durable, 0 incomplete, -1 error.
 * Caller must also recheck receipt.manifest against durable scalar values. */
int next_gateway_exchange_sequence(const char *mac,uint32_t token,uint16_t *seq);
int gateway_exchange_command_record(const char *mac,uint32_t token,uint8_t *frame,unsigned len,int save);
int get_completed_upload_batch(const char *mac, uint32_t batch, UploadBatchReceipt_t *receipt);

// 提取全量名册 (内部已通过 SQL 联表查询补齐了最新的 temp 和 mag)
int get_all_devices(DeviceRecord_t **out_records);
int get_confirmed_devices(DeviceRecord_t **out_records);
/* 1 newly confirmed, 0 already confirmed/stale/unknown address, -1 SQL error. */
int confirm_device_join(const char *mac, uint16_t short_addr);
int get_mac_by_short_addr(const char *short_addr, char *out_mac);

int update_device_work_mode(const char *mac, const char *work_mode);

int get_temperature_history(const char *mac,int history_seconds,TemperatureRecord_t **out_records);
int get_sound_history(const char *mac,int history_seconds,SoundRecord_t **out_records);

#define COLLECTION_HISTORY_LIMIT 30
typedef struct {
    uint32_t batch_id;
    long long timestamp;
    long long captured_at;
    float values[6]; /* temperature, mag_x/y/z, digital_db, analog_db */
    unsigned int present_mask;
} CollectionHistoryRecord_t;
/* Latest distinct acquisition batches for one metric, oldest first.
 * metric: temperature/digital/ultrasonic/magnetic. Returns -1 on error. */
int get_collection_history(const char *mac, const char *metric, int limit,
                           CollectionHistoryRecord_t **out_records);

int refresh_online_device_activity(void);
int cleanup_sensor_history(int retention_seconds);

#endif
