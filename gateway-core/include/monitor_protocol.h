#ifndef MONITOR_PROTOCOL_H
#define MONITOR_PROTOCOL_H

#include <stdint.h>

/* Linux 网关与协调器之间的串口命令。 */
#define GW_CMD_MONITOR_CONTROL       0x0BU
#define GW_CMD_MONITOR_CONFIG        0x0CU
#define GW_CMD_CHANNEL_CONFIG        0x0EU
#define GW_CMD_ADMISSION_GRANT       0x18U /* short[2] + attempt[4] */

/* GW_CMD_CHANNEL_CONFIG payload[3] control values. 0..65 keep the legacy
 * immediate migration delay. PREPARE moves only the listed awake terminals;
 * COMMIT performs the coordinator's final channel change. */
#define GW_CHANNEL_PHASE_PREPARE     0xFEU
#define GW_CHANNEL_PHASE_COMMIT      0xFFU

/* 协调器向 Linux 网关转发的终端上报。 */
#define GW_RSP_VIBRATION_CHUNK       0x8BU
#define GW_RSP_VIBRATION_SUMMARY     0x8CU
#define GW_RSP_MONITOR_STATE         0x8DU
#define GW_RSP_COMMAND_QUEUED        0x8EU
#define GW_RSP_DEVICE_LEFT           0x8FU
#define GW_RSP_AF_CONFIRM            0x90U
#define GW_RSP_AF_RX_TRACE           0x91U
#define GW_RSP_CHANNEL_CONFIG        0x96U
#define GW_RSP_CONFIG_QUERY          0x99U
#define GW_RSP_NETWORK_RESTORE       0x9AU
#define GW_RSP_RECOVERY_ACTIVITY     0xA0U /* IEEE[8], current short[2]. */

/* 协调器网络启动来源，以及0x9A持久化恢复结果。 */
#define GW_NETWORK_SOURCE_UNKNOWN    0x00U
#define GW_NETWORK_SOURCE_RESTORED   0x01U
#define GW_NETWORK_SOURCE_AUTO_FORM  0x02U
#define GW_RESTORE_NO_DATA           0x00U
#define GW_RESTORE_SUCCESS           0x01U
#define GW_RESTORE_FAILED            0x02U

/* 0x8A payload[8]: 协调器 Zigbee 网络生命周期状态。 */
#define GW_NETWORK_STATE_NOT_READY   0x00U
#define GW_NETWORK_STATE_RESTORING   0x01U
#define GW_NETWORK_STATE_SCANNING    0x02U
#define GW_NETWORK_STATE_FORMING     0x03U
#define GW_NETWORK_STATE_READY       0x04U
#define GW_NETWORK_STATE_ERROR       0x05U

/* Optional 0x8A payload[9] capability. The tagged 0x90 result appends
 * attempt[4] and stage[1] to the legacy short/cmd/status/ZCL sequence. */
#define GW_CAP_ADMISSION_GRANT       0x01U
#define GW_ADMISSION_RESULT_SIZE     10U
#define GW_ADMISSION_SUBMIT_FAILED   0U
#define GW_ADMISSION_AF_RESULT       1U

/*
 * 所有多字节整数均为小端。协调器只在终端载荷前添加 short_addr[2]：
 * 0x86: short[2] + batch[4] + temperature_centi[2]
 * 0x87: short[2] + batch[4] + mag_xyz[3][2]
 * 0x88/0x89: short[2] + batch[4] + db_spl_centi[2]
 * 0x8B: short[2] + batch[4] + packet_index[2] + packet_count[2] +
 *       sample_rate[2] + points[1] + xyz_int16[points][3]
 * 0x8C: short[2] + batch[4] + sample_rate[2] + sample_count[4] +
 *       features_milli[3][5][4]
 * 0x8D: short[2] + monitoring_enabled[1] + state[1] + batch[4]
 *       new coordinator appends power_source[1] + received LQI[1] + RSSI[1].
 *       Power uses Zigbee Basic values (0 unknown, 4 external DC); legacy 8-byte frames remain valid.
 * 0x8A: pan_id[2] + channel[1] + legacy_device_state[1] +
 *       startup_status[1] + startup_source[1] + restore_result[1] +
 *       restore_status[1] + network_state[1]
 */

/* GW_CMD_MONITOR_CONTROL 载荷中的操作码。 */
#define MONITOR_OP_SET_ENABLED       0x01U
#define MONITOR_OP_COLLECT_NOW       0x02U
#define MONITOR_OP_AUTHORIZE         0x03U

/* 终端采集状态。 */
#define MONITOR_STATE_SLEEP          0x00U
#define MONITOR_STATE_WAITING        0x01U
#define MONITOR_STATE_CAPTURING      0x02U
#define MONITOR_STATE_UPLOADING      0x03U
#define MONITOR_STATE_COMPLETE       0x04U
#define MONITOR_STATE_ERROR          0x05U

#define MONITOR_DEFAULT_AUTO_INTERVAL_SEC  300U
#define MONITOR_MIN_AUTO_INTERVAL_SEC       60U
#define MONITOR_DEFAULT_POLL_INTERVAL_SEC  5U
#define MONITOR_DEFAULT_VIBRATION_RATE_HZ  26667U
#define MONITOR_DEFAULT_DIGITAL_MIC_RATE_HZ 46875U
#define MONITOR_DEFAULT_ANALOG_MIC_RATE_HZ  256000U
#define MONITOR_DEFAULT_VIBRATION_WINDOW_MS 500U
#define MONITOR_DEFAULT_DIGITAL_MIC_WINDOW_MS 1000U
#define MONITOR_DEFAULT_ANALOG_MIC_WINDOW_MS 1000U
#define MONITOR_DEFAULT_RAW_UPLOAD_EVERY_CYCLES 1U
#define MONITOR_DEFAULT_POST_UPLOAD_RX_WINDOW_SEC 5U
#define MONITOR_SENSOR_VIBRATION              0x01U
#define MONITOR_SENSOR_DIGITAL_MIC             0x02U
#define MONITOR_SENSOR_ANALOG_MIC              0x04U
#define MONITOR_SENSOR_ALL                     0x07U

typedef struct
{
    uint32_t auto_interval_sec;
    uint16_t poll_interval_sec;
    uint16_t vibration_rate_hz;
    uint16_t digital_mic_rate_hz;
    uint32_t analog_mic_rate_hz;
    uint16_t vibration_window_ms;
    uint16_t digital_mic_window_ms;
    uint16_t analog_mic_window_ms;
    uint16_t raw_upload_every_cycles;
    uint32_t raw_upload_generation;
    uint16_t post_upload_rx_window_sec;
    uint8_t sensor_enable_mask;
} MonitorConfig_t;

#endif
