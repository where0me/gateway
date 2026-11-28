import { networkAddress } from './nodeManagementColumns.js';

// Keep the display-only boundary across page unmounts, without changing stored logs.
export function readLogClearBoundary(storage, hostname) {
    try {
        storage ??= window.sessionStorage;
        hostname ??= window.location.hostname;
        const value = Number(storage.getItem(`gateway-logs:hidden-through:${hostname}:8080`));
        return Number.isSafeInteger(value) && value >= 0 ? value : 0;
    } catch { return 0; }
}

export function saveLogClearBoundary(value, storage, hostname) {
    if (!Number.isSafeInteger(value) || value < 0) return;
    try {
        storage ??= window.sessionStorage;
        hostname ??= window.location.hostname;
        storage.setItem(`gateway-logs:hidden-through:${hostname}:8080`, String(value));
    } catch { /* Storage may be unavailable; clearing the mounted page still works. */ }
}
export function logParameters(filters) {
    if (filters.from && filters.to && new Date(filters.to) < new Date(filters.from)) throw new Error('结束时间不能早于开始时间');
    const params = new URLSearchParams({ limit: '100' });
    for (const [key, value] of Object.entries(filters)) if (value) {
        if (['from', 'to'].includes(key)) {
            const timestamp = new Date(value).getTime();
            if (!Number.isFinite(timestamp)) throw new Error('时间格式无效');
            params.set(key, String(Math.floor(timestamp / 1000)));
        } else params.set(key, value);
    }
    return params;
}
export const LOG_SOURCES = { backend: '后端', coordinator: '协调器', node: '节点', frontend: '用户操作' };
export const LOG_LEVELS = { INFO: '信息', WARN: '警告', ERROR: '错误', DEBUG: '调试' };
export const LOG_EVENTS = { system_start: '系统启动', system_stop: '系统停止', serial_open: '串口连接', serial_error: '串口异常', network_state: '网络状态',
    node_announced: '节点宣告', recovery_activity: '恢复活动', node_discovered: '发现节点', node_authorized: '授权节点', node_offline: '节点离线', node_left: '节点离网',
    sampling_saved: '配置保存', sampling_submitted: '配置提交', sampling_failed: '配置提交失败', collection_state: '采集状态', waveform_saved: '波形保存',
    features_saved: '特征保存', sensor_capture_missing: '采集缺项', control_retry: '控制命令重发', monitor_applied: '配置已生效', upload_reservation: '上传名额', removal_pending: '等待清退', removal_retry: '提交离网', removal_completed: '清退完成',
    upload_trace_snapshot: '上传链路快照', upload_ack_failure: '上传回复失败', upload_ack_submit: '上传回复提交', upload_ack_result: '上传回复投递结果',
    radio_mac_result: 'MAC发送结果', radio_diag_coverage: '无线观察范围', radio_diag_dropped: '无线观察丢失',
    command_queue_failed: '命令入队失败', delivery_failed: '无线投递失败', name_changed: '修改名称', description_changed: '修改描述', permit_join_requested: '请求开放入网' };
export function logsCsv(rows) {
    const cell = value => `"${String(value ?? '').replace(/^[\s]*[=+@-]/, "'$&").replaceAll('"', '""')}"`;
    return '\uFEFF' + [['时间', '等级', '来源', 'MAC地址', '网络地址', '批次', '事件', '说明'], ...rows.map(row => [
        new Date(row.timestamp * 1000).toLocaleString('zh-CN', { hour12: false }), LOG_LEVELS[row.level] || row.level,
        LOG_SOURCES[row.source] || row.source, row.mac, networkAddress(row.short_addr)?.hex, row.batch,
        LOG_EVENTS[row.event] || row.event, row.message
    ])].map(row => row.map(cell).join(',')).join('\r\n');
}
