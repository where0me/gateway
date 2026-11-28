import React, { useCallback, useEffect, useRef, useState } from 'react';
import './App.css';
import './NodeSamplingCard.css';
import CompactNodeCard, { WaveformPage } from './NodeDetails.jsx';
import NodeDetailsList from './NodeDetailsList.jsx';
import HomeNodes from './HomeNodes.jsx';
import PendingAdmissionPanel from './PendingAdmissionPanel.jsx';
import NodeActions from './NodeActions.jsx';
import NodeProfile from './NodeProfile.jsx';
import GatewayLogs from './GatewayLogs.jsx';
import NodeGroups from './NodeGroups.jsx';
import GatewaySidebar from './GatewaySidebar.jsx';
import SettingsPage from './SettingsPage.jsx';
import { interviewLabel, interviewSummary } from './nodeInterview.js';
import { mergeDeviceSnapshot, mergeLiveCollection, mergeVibrationDetails } from './nodeDetailsState.js';
import { createMonitoringSwitchState } from './monitoringSwitchState.js';
import { useHomeActivity } from './homeActivity.js';
import { NodeColumnSelector, NodeManagementField } from './NodeManagementFields.jsx';
import { COLUMN_STORAGE_KEY, NODE_COLUMNS, readNodeColumns } from './nodeManagementColumns.js';
import { Network, Wifi, WifiOff, RadioTower, CircleStop, Plus, Clock3, Activity, Mic, AudioWaveform, LockKeyhole, Save, LoaderCircle, CircleCheck, AlertCircle } from 'lucide-react';


const TEMP_LIMIT = 45.0;
const TEMP_HISTORY_MINUTES = 30;
const TEMP_HISTORY_SECONDS = TEMP_HISTORY_MINUTES * 60;
const TEMP_HISTORY_TIME = TEMP_HISTORY_SECONDS * 1000;
const TEMP_TREND_GAP_MS = 30 * 1000;
const SOUND_TREND_GAP_MS = 15 * 1000;
const SOUND_TREND_BUCKET_MS = 5 * 1000;
const MIN_VALID_GATEWAY_TIME_MS = Date.UTC(2024, 0, 1);
const SENSOR_WINDOW_OPTIONS = [0.5, 1, 1.5];
const MICROPHONE_WINDOW_OPTIONS = [0.5, 1, 2];
const MONITOR_INTERVAL_OPTIONS = [1, 2, 3, 4, 5, 10, 20, 30, 60];
const RAW_UPLOAD_CYCLE_OPTIONS = [1, 2, 3, 5, 10];
const DEFAULT_MONITOR_POLICY_ID = 'default';

const createMonitorPolicy = ({
    id = DEFAULT_MONITOR_POLICY_ID,
    name = '默认采样配置',
    isDefault = true
} = {}) => ({
    id,
    name,
    isDefault,
    automatic_interval_minutes: 5,
    vibration_sampling_rate: 26667,
    digital_microphone_sampling_rate: 46875,
    analog_microphone_sampling_rate: 256000,
    vibration_window_seconds: 0.5,
    digital_microphone_window_seconds: 1,
    analog_microphone_window_seconds: 1,
    raw_upload_every_cycles: 1,
    vibration_enabled: true,
    digital_microphone_enabled: true,
    analog_microphone_enabled: true
});


const collectionStateText = state => ({
    idle: '',
    waiting: '等待下一次自动采集',
    capturing: '正在采集',
    uploading: '正在上传振动数据',
    complete: '上传完成',
    error: '采集失败'
}[state] || '');


const formatCollectionTime = epochSeconds => {
    const value = Number(epochSeconds);
    return Number.isFinite(value) && value > 0
        ? new Date(value * 1000).toLocaleString('zh-CN', { hour12: false })
        : '';
};




const formatStatusTime = value => value
    ? new Date(value).toLocaleString('zh-CN', { hour12: false })
    : '--';


const formatResourceBytes = value => {
    const bytes = Number(value);
    if (!Number.isFinite(bytes) || bytes < 0) return '--';
    if (bytes < 1024) return `${Math.round(bytes)} B`;

    const units = ['KB', 'MB', 'GB', 'TB'];
    let scaled = bytes / 1024;
    let unitIndex = 0;
    while (scaled >= 1024 && unitIndex < units.length - 1)
    {
        scaled /= 1024;
        unitIndex += 1;
    }
    return `${scaled >= 100 ? scaled.toFixed(0) : scaled.toFixed(1)} ${units[unitIndex]}`;
};


const formatGatewayUptime = value => {
    const totalSeconds = Number(value);
    if (!Number.isFinite(totalSeconds) || totalSeconds < 0) return '--';

    const seconds = Math.floor(totalSeconds);
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    if (days > 0) return `${days}天 ${hours}小时 ${minutes}分钟`;
    if (hours > 0) return `${hours}小时 ${minutes}分钟`;
    return `${minutes}分钟`;
};


const zigbeeNetworkStateText = state => ({
    not_ready: '网络未就绪',
    restoring: '正在恢复网络',
    scanning: '正在扫描信道',
    forming: '正在建立网络',
    ready: '网络已就绪',
    error: '建网失败',
    unknown: '等待网络状态'
}[state] || '等待网络状态');


function StatusMetric({ title, value, hint, tone = 'neutral' })
{
    const colors = {
        success: { background: '#edf9f3', border: '#bce8d0', value: '#188a55' },
        danger: { background: '#fff1f0', border: '#f3c4c0', value: '#c0392b' },
        warning: { background: '#fff8e8', border: '#f0d99e', value: '#ad7610' },
        neutral: { background: '#f7f9fb', border: '#e1e7ed', value: '#26384a' }
    };
    const palette = colors[tone] || colors.neutral;

    return (
        <div style={{
            minHeight: '112px',
            padding: '18px',
            borderRadius: '10px',
            boxSizing: 'border-box',
            background: palette.background,
            border: `1px solid ${palette.border}`
        }}>
            <div style={{ color: '#7b8794', fontSize: '13px' }}>{title}</div>
            <div style={{
                marginTop: '10px',
                color: palette.value,
                fontSize: '21px',
                lineHeight: 1.25,
                fontWeight: 750,
                wordBreak: 'break-word'
            }}>
                {value}
            </div>
            {hint && (
                <div style={{ marginTop: '8px', color: '#8a98a6', fontSize: '12px' }}>
                    {hint}
                </div>
            )}
        </div>
    );
}


export function NodeSamplingCard({ policy, notice, busy, onFieldChange, onSave })
{
    const sensors = [
        { key: 'vibration', name: '振动', Icon: Activity, field: 'vibration_enabled',
            window: 'vibration_window_seconds', rate: '26.667 kHz', options: SENSOR_WINDOW_OPTIONS },
        { key: 'digital', name: '数字麦克风', Icon: Mic, field: 'digital_microphone_enabled',
            window: 'digital_microphone_window_seconds', rate: '46.875 kHz', options: MICROPHONE_WINDOW_OPTIONS },
        { key: 'ultrasound', name: '超声麦克风', Icon: AudioWaveform, field: 'analog_microphone_enabled',
            window: 'analog_microphone_window_seconds', rate: '256 kHz', options: MICROPHONE_WINDOW_OPTIONS }
    ];
    const failed = /失败|错误/.test(notice || '');
    const StatusIcon = failed ? AlertCircle : notice ? CircleCheck : Clock3;
    return (
        <div className="node-sampling-card">
            <fieldset disabled={busy} className="node-sampling-fieldset" aria-busy={busy}>
                <div className="node-sampling-intro">
                    <h3>采样参数</h3>
                    <p>仅编辑当前节点，不影响其他节点。</p>
                </div>
                <div className="monitor-policy-groups">
                    <section className="monitor-policy-group node-sampling-group is-cycle">
                        <header className="node-sampling-group-heading">
                            <span className="node-sampling-group-icon"><Clock3 size={18} aria-hidden="true" /></span>
                            <h3>采集与上传周期</h3>
                        </header>
                        <label className="node-sampling-field">
                            <span>自动采集周期</span>
                            <select aria-label="自动采集周期" value={policy.automatic_interval_minutes}
                                onChange={event => onFieldChange(policy.id, 'automatic_interval_minutes', Number(event.target.value))}>
                                {!MONITOR_INTERVAL_OPTIONS.includes(policy.automatic_interval_minutes) &&
                                    <option value={policy.automatic_interval_minutes}>{policy.automatic_interval_minutes}分钟（当前配置）</option>}
                                {MONITOR_INTERVAL_OPTIONS.map(value => <option key={value} value={value}>{value}分钟</option>)}
                            </select>
                        </label>
                        <label className="node-sampling-field">
                            <span>原始振动波形上传周期</span>
                            <select aria-label="原始振动波形上传周期" value={policy.raw_upload_every_cycles} disabled={!policy.vibration_enabled}
                                onChange={event => onFieldChange(policy.id, 'raw_upload_every_cycles', Number(event.target.value))}>
                                {!RAW_UPLOAD_CYCLE_OPTIONS.includes(policy.raw_upload_every_cycles) &&
                                    <option value={policy.raw_upload_every_cycles}>每 {policy.raw_upload_every_cycles} 个周期（当前配置）</option>}
                                {RAW_UPLOAD_CYCLE_OPTIONS.map(value =>
                                    <option key={value} value={value}>{value === 1 ? '每个周期上传' : `每 ${value} 个周期上传一次`}</option>)}
                            </select>
                        </label>
                        <p className="node-sampling-hint">{policy.vibration_enabled
                            ? '首次生效必定上传；更换配置或修改此项时计数清零，修改其他参数不清零。'
                            : '振动采集已关闭，不会生成或上传原始振动波形。'}</p>
                    </section>
                    {sensors.map(({ key, name, Icon, field, window, rate, options }) => (
                        <section key={key} className={`monitor-policy-group node-sampling-group is-${key}${policy[field] ? '' : ' is-disabled'}`}>
                            <header className="node-sampling-group-heading">
                                <span className="node-sampling-group-icon"><Icon size={18} aria-hidden="true" /></span>
                                <h3>{name}</h3>
                                <label className="node-sampling-toggle">
                                    <input type="checkbox" role="switch" aria-label={`${name}采集`} checked={policy[field]}
                                        onChange={event => onFieldChange(policy.id, field, event.target.checked)} />
                                    <span className="node-sampling-switch" aria-hidden="true" />
                                    <span>{policy[field] ? '已开启' : '已关闭'}</span>
                                </label>
                            </header>
                            <label className="node-sampling-field">
                                <span>采样率 <small>固定</small></span>
                                <span className="node-sampling-readonly">
                                    <input readOnly tabIndex={-1} aria-label={`${name}采样率（固定）`}
                                        value={rate} disabled={!policy[field]} />
                                    <LockKeyhole size={14} aria-hidden="true" />
                                </span>
                            </label>
                            <label className="node-sampling-field">
                                <span>采集窗口</span>
                                <select aria-label={`${name}采集窗口`} value={policy[window]} disabled={!policy[field]}
                                    onChange={event => onFieldChange(policy.id, window, Number(event.target.value))}>
                                    {options.map(value => <option key={value} value={value}>{value}秒</option>)}
                                    {key !== 'vibration' && policy[window] === 1.5 && <option value={1.5}>1.5秒（旧配置）</option>}
                                </select>
                            </label>
                        </section>
                    ))}
                </div>
                <footer className="node-sampling-footer">
                    <span className={`node-sampling-status${failed ? ' is-error' : notice ? ' is-success' : ''}`} role="status" aria-live="polite">
                        <StatusIcon size={16} aria-hidden="true" />
                        {notice || '保存后，节点在下次唤醒或上传结束查询时获取新参数。'}
                    </span>
                    <button type="button" className="node-sampling-save" onClick={() => onSave(policy.id)}>
                        {busy ? <LoaderCircle size={16} className="node-sampling-spinner" aria-hidden="true" />
                            : <Save size={16} aria-hidden="true" />}
                        {busy ? '保存中…' : '保存配置'}
                    </button>
                </footer>
            </fieldset>
        </div>
    );
}


/* ============================================================
 * 公共样式
 * ============================================================ */
const styles = {
    container: {
        display: 'flex',
        height: '100vh',
        overflow: 'hidden',
        fontFamily: 'system-ui, sans-serif',
        backgroundColor: '#f5f7fa'
    },

    sidebar: {
        width: '220px',
        height: '100vh',
        flexShrink: 0,
        padding: '20px 0',
        boxSizing: 'border-box',
        backgroundColor: '#2c3e50',
        color: 'white'
    },

    sidebarTitle: {
        padding: '0 20px',
        color: '#bdc3c7'
    },

    menuItem: {
        padding: '15px 20px',
        cursor: 'pointer',
        transition: '0.3s'
    },

    subMenuItem: {
        padding: '11px 20px 11px 43px',
        borderLeft: '4px solid transparent',
        boxSizing: 'border-box',
        cursor: 'pointer',
        color: '#c8d3dc',
        fontSize: '14px',
        transition: '0.2s'
    },

    activeSubMenu: {
        color: '#ffffff',
        backgroundColor: '#243648',
        borderLeftColor: '#5dade2'
    },

    activeMenu: {
        backgroundColor: '#34495e',
        borderLeft: '4px solid #3498db'
    },

    content: {
        flex: 1,
        height: '100vh',
        minWidth: 0,
        padding: '30px',
        boxSizing: 'border-box',
        overflowY: 'auto'
    },

    card: {
        padding: '20px',
        backgroundColor: 'white',
        borderRadius: '8px',
        boxShadow: '0 2px 12px rgba(0,0,0,0.1)'
    },

    cardHeader: {
        display: 'flex',
        justifyContent: 'space-between',
        alignItems: 'center',
        gap: '15px'
    },

    macText: {
        marginTop: '14px',
        fontSize: '13px',
        color: '#7f8c8d',
        wordBreak: 'break-all'
    },

    statusGrid: {
        display: 'grid',
        gridTemplateColumns: 'minmax(0, 1fr)',
        gap: '14px',
        marginTop: '20px'
    },

    statusBox: {
        minHeight: '72px',
        padding: '12px',
        borderRadius: '8px',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        gap: '10px',
        fontWeight: 'bold',
        fontSize: '14px'
    },

    detailToggle: {
        marginTop: '14px',
        textAlign: 'right',
        fontSize: '12px',
        color: '#95a5a6'
    },

    detailArea: {
        marginTop: '18px',
        paddingTop: '22px',
        borderTop: '1px solid #e5e8e8'
    },

    sectionHeader: {
        marginBottom: '14px',
        fontSize: '16px',
        fontWeight: 'bold',
        color: '#34495e'
    },

    infoGrid2: {
        display: 'grid',
        gridTemplateColumns: 'repeat(2, minmax(0, 1fr))',
        gap: '10px'
    },

    infoGrid3: {
        display: 'grid',
        gridTemplateColumns: 'repeat(3, minmax(0, 1fr))',
        gap: '10px'
    },

    infoBox: {
        padding: '14px',
        backgroundColor: '#f8f9fa',
        borderRadius: '8px'
    },

    infoTitle: {
        fontSize: '12px',
        color: '#95a5a6'
    },

    infoValue: {
        marginTop: '6px',
        fontWeight: 'bold',
        color: '#34495e',
        wordBreak: 'break-all'
    },

    button: {
        padding: '10px 20px',
        color: 'white',
        border: 'none',
        borderRadius: '4px',
        cursor: 'pointer',
        marginRight: '10px'
    },

    input: {
        width: '250px',
        padding: '9px',
        marginRight: '10px',
        borderRadius: '4px',
        border: '1px solid #ccc'
    },

    table: {
        width: '100%',
        marginTop: '15px',
        borderCollapse: 'collapse'
    },

    th: {
        padding: '12px',
        textAlign: 'left',
        color: '#7f8c8d',
        borderBottom: '2px solid #eee'
    },

    td: {
        padding: '12px',
        borderBottom: '1px solid #eee'
    },

    emptyState: {
        padding: '40px',
        textAlign: 'center',
        color: '#95a5a6'
    }
};


/* ============================================================
 * 状态框
 * ============================================================ */


/* ============================================================
 * 详情信息框
 * ============================================================ */




/* ============================================================
 * 基于真实时间戳的传感器趋势图
 * ============================================================ */
function downsampleHistory(data, valueKey, bucketMs)
{
    if (!bucketMs || bucketMs <= 0)
    {
        return data;
    }

    const buckets = new Map();

    data.forEach(item => {
        const bucketStart = Math.floor(item.time / bucketMs) * bucketMs;
        const current = buckets.get(bucketStart) || {
            timeSum: 0,
            valueSum: 0,
            count: 0
        };

        current.timeSum += item.time;
        current.valueSum += Number(item[valueKey]);
        current.count += 1;
        buckets.set(bucketStart, current);
    });

    return Array.from(buckets.values()).map(bucket => ({
        time: bucket.timeSum / bucket.count,
        [valueKey]: bucket.valueSum / bucket.count
    }));
}




function VibrationWaveform({ dev, samplingRate })
{
    const width = 900;
    const height = 300;
    const left = 56;
    const right = 22;
    const top = 24;
    const bottom = 40;
    const graphWidth = width - left - right;
    const graphHeight = height - top - bottom;
    const samples = Array.isArray(dev.vibration_samples) ? dev.vibration_samples : [];
    const sampleCount = Number(dev.vibration_sample_count) || samples.length;
    const windowSeconds = sampleCount > 0 && samplingRate > 0
        ? sampleCount / samplingRate : 0;
    if (samples.length < 2)
    {
        return (
            <div
                onClick={event => event.stopPropagation()}
                style={{
                    marginTop: '14px',
                    padding: '28px 16px',
                    border: '1px dashed #d7e0e8',
                    borderRadius: '10px',
                    backgroundColor: '#fbfcfe',
                    color: '#7b8794',
                    textAlign: 'center'
                }}
            >
                暂无真实振动波形，完成首次采集后显示
            </div>
        );
    }
    const renderStep = samples.length > 1200 ? Math.ceil(samples.length / 1200) : 1;
    const displaySamples = renderStep === 1
        ? samples
        : samples.filter((_, index) => index % renderStep === 0 || index === samples.length - 1);
    const axes = [
        { key: 'x', label: 'X', color: '#2d7ff9' },
        { key: 'y', label: 'Y', color: '#15a374' },
        { key: 'z', label: 'Z', color: '#ee7b30' }
    ];
    const values = displaySamples.flatMap(sample => axes.map(axis => Number(sample[axis.key]) || 0));
    const maxAbs = Math.max(0.1, ...values.map(value => Math.abs(value))) * 1.12;
    const xOf = index => left + (index / (displaySamples.length - 1)) * graphWidth;
    const yOf = value => top + (1 - (value + maxAbs) / (2 * maxAbs)) * graphHeight;

    const downloadCsv = event => {
        event.stopPropagation();
        const rows = ['sample_index,time_s,x_raw,y_raw,z_raw'];
        samples.forEach((sample, index) => {
            rows.push([
                sample.index ?? index,
                Number(sample.time ?? index / samplingRate).toFixed(8),
                sample.x_raw ?? Number(sample.x).toFixed(6),
                sample.y_raw ?? Number(sample.y).toFixed(6),
                sample.z_raw ?? Number(sample.z).toFixed(6)
            ].join(','));
        });
        const blob = new Blob([`\uFEFF${rows.join('\n')}`], {
            type: 'text/csv;charset=utf-8'
        });
        const url = URL.createObjectURL(blob);
        const anchor = document.createElement('a');
        anchor.href = url;
        anchor.download = `${String(dev.name || dev.mac || 'node').replace(/[^\w\u4e00-\u9fa5-]/g, '_')}-vibration.csv`;
        anchor.click();
        URL.revokeObjectURL(url);
    };

    return (
        <div
            onClick={event => event.stopPropagation()}
            style={{
                marginTop: '14px',
                padding: '16px',
                border: '1px solid #e8edf2',
                borderRadius: '10px',
                backgroundColor: '#fbfcfe'
            }}
        >
            <div
                style={{
                    display: 'flex',
                    justifyContent: 'space-between',
                    alignItems: 'center',
                    gap: '12px',
                    flexWrap: 'wrap',
                    marginBottom: '10px'
                }}
            >
                <div>
                    <strong style={{ color: '#26384a' }}>最近一次三轴振动波形</strong>
                    <div style={{ marginTop: '4px', color: '#7b8794', fontSize: '12px' }}>
                        {windowSeconds.toFixed(1)}秒窗口 · {samplingRate.toLocaleString()} Hz · {sampleCount.toLocaleString()}点/轴
                    </div>
                </div>
                <div style={{ display: 'flex', alignItems: 'center', gap: '10px' }}>
                    <button
                        type="button"
                        onClick={downloadCsv}
                        style={{
                            ...styles.button,
                            margin: 0,
                            padding: '8px 12px',
                            backgroundColor: '#34495e'
                        }}
                    >
                        下载CSV
                    </button>
                </div>
            </div>

            <div style={{ display: 'flex', gap: '16px', marginBottom: '4px' }}>
                {axes.map(axis => (
                    <span key={axis.key} style={{ color: axis.color, fontSize: '12px', fontWeight: 700 }}>
                        ● {axis.label}轴
                    </span>
                ))}
            </div>

            <div style={{ overflowX: 'auto' }}>
                <svg viewBox={`0 0 ${width} ${height}`} style={{ width: '100%', minWidth: '620px', display: 'block' }}>
                    {[0, 0.25, 0.5, 0.75, 1].map(ratio => {
                        const y = top + ratio * graphHeight;
                        const value = maxAbs - ratio * maxAbs * 2;
                        return (
                            <g key={ratio}>
                                <line x1={left} x2={width - right} y1={y} y2={y} stroke="#e4e9ee" />
                                <text x={left - 9} y={y + 4} textAnchor="end" fontSize="11" fill="#7b8794">
                                    {value.toFixed(1)}g
                                </text>
                            </g>
                        );
                    })}
                    {[0, 0.25, 0.5, 0.75, 1].map(ratio => (
                        <g key={`x-${ratio}`}>
                            <line
                                x1={left + ratio * graphWidth}
                                x2={left + ratio * graphWidth}
                                y1={top}
                                y2={height - bottom}
                                stroke="#eef1f4"
                            />
                            <text
                                x={left + ratio * graphWidth}
                                y={height - 14}
                                textAnchor="middle"
                                fontSize="11"
                                fill="#7b8794"
                            >
                                {ratio.toFixed(2)}s
                            </text>
                        </g>
                    ))}
                    {axes.map(axis => (
                        <polyline
                            key={axis.key}
                            fill="none"
                            stroke={axis.color}
                            strokeWidth="2"
                            strokeLinejoin="round"
                            points={displaySamples.map((sample, index) => `${xOf(index)},${yOf(Number(sample[axis.key]) || 0)}`).join(' ')}
                        />
                    ))}
                </svg>
            </div>
        </div>
    );
}


function TimeSeriesTrend({
    data,
    valueKey,
    unit,
    color,
    emptyText,
    valuePadding,
    gapThresholdMs,
    windowEnd,
    downsampleMs = 0
})
{
    const width = 700;
    const height = 220;
    const paddingLeft = 55;
    const paddingRight = 20;
    const paddingTop = 20;
    const paddingBottom = 35;
    const now = windowEnd;
    const minTime = now - TEMP_HISTORY_TIME;

    const validData = (data || [])
        .map(item => ({
            ...item,
            time: Number(item.time),
            [valueKey]: Number(item[valueKey])
        }))
        .filter(item =>
            Number.isFinite(item.time) &&
            Number.isFinite(item[valueKey]) &&
            item.time >= minTime &&
            item.time <= now + 1000
        )
        .sort((a, b) => a.time - b.time);

    const chartData = downsampleHistory(validData, valueKey, downsampleMs);

    if (chartData.length === 0)
    {
        return (
            <div
                style={{
                    height: '220px',
                    display: 'flex',
                    justifyContent: 'center',
                    alignItems: 'center',
                    backgroundColor: '#f8f9fa',
                    borderRadius: '8px',
                    color: '#95a5a6',
                    fontSize: '14px'
                }}
            >
                {emptyText}
            </div>
        );
    }

    const values = chartData.map(item => Number(item[valueKey]));

    let minValue = Math.min(...values);
    let maxValue = Math.max(...values);

    if (minValue === maxValue)
    {
        minValue -= valuePadding;
        maxValue += valuePadding;
    }
    else
    {
        minValue -= valuePadding / 2;
        maxValue += valuePadding / 2;
    }

    const graphWidth = width - paddingLeft - paddingRight;
    const graphHeight = height - paddingTop - paddingBottom;

    const plotPoints = chartData.map(item => {
        const x = paddingLeft +
            ((item.time - minTime) / TEMP_HISTORY_TIME) * graphWidth;
        const y =
            paddingTop +
            (
                1 -
                (Number(item[valueKey]) - minValue) /
                (maxValue - minValue)
            ) * graphHeight;

        return { ...item, x, y };
    });

    const segments = [];
    let currentSegment = [];

    plotPoints.forEach(point => {
        const previous = currentSegment[currentSegment.length - 1];

        if (previous && point.time - previous.time > gapThresholdMs)
        {
            segments.push(currentSegment);
            currentSegment = [];
        }

        currentSegment.push(point);
    });

    if (currentSegment.length > 0)
    {
        segments.push(currentSegment);
    }

    const yTicks = [];

    for (let i = 0; i < 5; i++)
    {
        const ratio = i / 4;

        yTicks.push({
            value: maxValue - ratio * (maxValue - minValue),
            y: paddingTop + ratio * graphHeight
        });
    }

    const latest = plotPoints[plotPoints.length - 1];
    const formatTick = value => unit === '℃'
        ? `${value.toFixed(1)}℃`
        : `${value.toFixed(1)} ${unit}`;

    return (
        <div style={{ width: '100%', overflowX: 'auto' }}>
            <svg
                viewBox={`0 0 ${width} ${height}`}
                style={{
                    width: '100%',
                    minWidth: '420px',
                    height: '220px',
                    display: 'block',
                    backgroundColor: '#f8f9fa',
                    borderRadius: '8px'
                }}
            >
                {yTicks.map((tick, index) => (
                    <g key={index}>
                        <line
                            x1={paddingLeft}
                            y1={tick.y}
                            x2={width - paddingRight}
                            y2={tick.y}
                            stroke="#e5e8e8"
                        />

                        <text
                            x={paddingLeft - 10}
                            y={tick.y + 4}
                            textAnchor="end"
                            fontSize="11"
                            fill="#7f8c8d"
                        >
                            {formatTick(tick.value)}
                        </text>
                    </g>
                ))}

                <line
                    x1={paddingLeft}
                    y1={height - paddingBottom}
                    x2={width - paddingRight}
                    y2={height - paddingBottom}
                    stroke="#bdc3c7"
                />

                <line
                    x1={paddingLeft}
                    y1={paddingTop}
                    x2={paddingLeft}
                    y2={height - paddingBottom}
                    stroke="#bdc3c7"
                />

                {segments.map((segment, index) => (
                    segment.length >= 2 ? (
                        <polyline
                            key={`line-${index}`}
                            fill="none"
                            stroke={color}
                            strokeWidth="3"
                            strokeLinejoin="round"
                            strokeLinecap="round"
                            points={segment.map(point => `${point.x},${point.y}`).join(' ')}
                        />
                    ) : segment[0] !== latest ? (
                        <circle
                            key={`point-${index}`}
                            cx={segment[0].x}
                            cy={segment[0].y}
                            r="4"
                            fill={color}
                        />
                    ) : null
                ))}

                <circle
                    cx={latest.x}
                    cy={latest.y}
                    r="5"
                    fill={color}
                />

                {chartData.length === 1 && (
                    <text
                        x={width / 2}
                        y={paddingTop + 18}
                        textAnchor="middle"
                        fontSize="12"
                        fill="#95a5a6"
                    >
                        已收到 1 个数据点，正在积累趋势
                    </text>
                )}

                <text
                    x={paddingLeft}
                    y={height - 10}
                    fontSize="11"
                    fill="#95a5a6"
                >
                    {TEMP_HISTORY_MINUTES} 分钟前
                </text>

                <text
                    x={paddingLeft + graphWidth / 2}
                    y={height - 10}
                    textAnchor="middle"
                    fontSize="11"
                    fill="#95a5a6"
                >
                    {TEMP_HISTORY_MINUTES / 2} 分钟前
                </text>

                <text
                    x={width - paddingRight}
                    y={height - 10}
                    textAnchor="end"
                    fontSize="11"
                    fill="#95a5a6"
                >
                    当前
                </text>
            </svg>
        </div>
    );
}


function _TemperatureTrend({ data, windowEnd })
{
    return (
        <TimeSeriesTrend
            data={data}
            valueKey="temp"
            unit="℃"
            color="#3498db"
            emptyText="正在积累温度趋势数据..."
            valuePadding={1.0}
            gapThresholdMs={TEMP_TREND_GAP_MS}
            windowEnd={windowEnd}
        />
    );
}


function _SoundTrend({ data, source, windowEnd })
{
    const isDigital = source === 'digital';

    return (
        <TimeSeriesTrend
            data={data}
            valueKey="db"
            unit="dB SPL"
            color={isDigital ? '#8e44ad' : '#e67e22'}
            emptyText={`正在积累${isDigital ? '数字' : '模拟'}麦克风趋势数据...`}
            valuePadding={4.0}
            gapThresholdMs={SOUND_TREND_GAP_MS}
            windowEnd={windowEnd}
            downsampleMs={SOUND_TREND_BUCKET_MS}
        />
    );
}


/* ============================================================
 * 获取节点健康状态
 * ============================================================ */


/* ============================================================
 * 磁场解析
 * ============================================================ */
function parseMagData(mag)
{
    if (!mag || mag === '--')
    {
        return null;
    }

    if (typeof mag === 'object')
    {
        return {
            x: mag.x ?? mag.X ?? '--',
            y: mag.y ?? mag.Y ?? '--',
            z: mag.z ?? mag.Z ?? '--',
            raw: null
        };
    }

    const text = String(mag);

    const xMatch = text.match(/X\s*[:=]\s*(-?[\d.]+)/i);
    const yMatch = text.match(/Y\s*[:=]\s*(-?[\d.]+)/i);
    const zMatch = text.match(/Z\s*[:=]\s*(-?[\d.]+)/i);

    if (xMatch && yMatch && zMatch)
    {
        return {
            x: xMatch[1],
            y: yMatch[1],
            z: zMatch[1],
            raw: null
        };
    }

    return {
        raw: text
    };
}


/* ============================================================
 * 单个节点卡片
 * ============================================================ */
function NodeCard(props)
{
    return <CompactNodeCard {...props} parseMagData={parseMagData} />;
}


/* ============================================================
 * 主程序
 * ============================================================ */
export default function App()
{
    return new URLSearchParams(window.location.search).has('waveform')
        ? <WaveformPage Waveform={VibrationWaveform} /> : <GatewayApp />;
}

function GatewayApp()
{
    const monitorSwitches = useRef(createMonitoringSwitchState());
    const [admissionRevision, setAdmissionRevision] = useState(0);
    const refreshAuthorizedNodes = useCallback(() => setAdmissionRevision(previous => previous + 1), []);
    const [activeTab, setActiveTab] = useState('home');
    const [gatewayStatus, setGatewayStatus] = useState({
        apiConnected: null,
        wsConnected: false,
        lastApiCheck: null,
        serverTimeMs: null,
        serverTimeCapturedAt: null,
        timeAligned: null,
        timeOffsetMs: null,
        timeLastCheck: null,
        timeSyncError: ''
    });
    const [gatewayClockTick, setGatewayClockTick] = useState(() => Date.now());
    const [systemResources, setSystemResources] = useState({
        data: null,
        lastUpdated: null,
        error: ''
    });
    const [whitelist, setWhitelist] = useState([]);
    const [nodeColumns, setNodeColumns] = useState(readNodeColumns);
    const [nodeLastUplink, setNodeLastUplink] = useState({});
    const changeNodeColumn = (key, checked) => {
        const next = { ...nodeColumns, [key]: checked };
        if (!Object.values(next).some(Boolean)) return;
        setNodeColumns(next);
        try { localStorage.setItem(COLUMN_STORAGE_KEY, JSON.stringify(next)); } catch { /* Keep session preference. */ }
    };
    const homeActivity = useHomeActivity(whitelist);
    const recordHomeActivity = homeActivity.record;
    const [joinState, setJoinState] = useState({ known: false, active: false, pending: false, remaining: 0 });
    const [joinIntent, setJoinIntent] = useState(null);
    const isScanning = joinState.active;
    const showPendingAdmissions = isScanning && joinIntent !== false;
    const [manualMac, setManualMac] = useState('');
    const [selectedTrend, setSelectedTrend] = useState(null);
    const [_tempHistory, setTempHistory] = useState({});
    const [_soundHistory, setSoundHistory] = useState({});
    const [_trendWindowEnd, setTrendWindowEnd] = useState(() => Date.now());
    const [windowWidth, setWindowWidth] = useState(window.innerWidth);
    const [renamingMac, setRenamingMac] = useState(null);
    const [renameDraft, setRenameDraft] = useState('');
    const [savingNameMac, setSavingNameMac] = useState(null);
    const [systemSettings, setSystemSettings] = useState({
        channel_strategy: 'auto',
        target_channel: null,
        pan_id: null,
        current_channel: null,
        network_source: 'unknown',
        coordinator_online: false,
        serial_available: null,
        coordinator_last_seen_ms: null,
        network_state: 'unknown',
        channel_operation: { state: 'idle' }
    });
    const [settingsLoading, setSettingsLoading] = useState(false);
    const [channelDraft, setChannelDraft] = useState('auto');
    const channelDraftEdited = useRef(false);
    const [channelAction, setChannelAction] = useState(false);
    const [channelSaveNotice, setChannelSaveNotice] = useState('');
    const [, setNodeMonitoring] = useState({});
    const [collectionStatus, setCollectionStatus] = useState({});
    const [monitorPolicies, setMonitorPolicies] = useState(() => [
        createMonitorPolicy()
    ]);
    const [samplingNode, setSamplingNode] = useState(null);
    const [samplingDraft, setSamplingDraft] = useState(null);
    const [samplingNotice, setSamplingNotice] = useState('');
    const [samplingLoading, setSamplingLoading] = useState(false);
    const [samplingSaving, setSamplingSaving] = useState(false);
    const samplingRequest = useRef(0);
    const defaultMonitoringPolicy = monitorPolicies.find(policy =>
        policy.id === DEFAULT_MONITOR_POLICY_ID
    ) || createMonitorPolicy();


    const vibrationRequests = useRef(new Map());
    const deviceEventVersions = useRef(new Map());
    const loadLatestVibration = useCallback((mac, includeRaw = true, isCurrent = () => true, batchId) => {
        const token = {};
        vibrationRequests.current.set(mac, token);
        const query = new URLSearchParams({ mac, raw: includeRaw ? '1' : '0' });
        if (Number(batchId) > 0) query.set('batch', String(batchId));
        return fetch(`http://${window.location.hostname}:8080/api/device/vibration/latest?${query}`, { cache: 'no-store' })
            .then(async response => {
                if (response.status === 404) return null;
                const data = await response.json();
                if (!response.ok || data.status !== 'success')
                    throw new Error(data.msg || `HTTP ${response.status}`);
                return data;
            })
            .then(data => {
                if (!data || !isCurrent() || vibrationRequests.current.get(mac) !== token) return;
                setWhitelist(previous => previous.map(device => device.mac === mac
                    ? mergeVibrationDetails(device, data, formatCollectionTime) : device));
            })
            .catch(error => console.error(`加载节点 ${mac} 振动数据失败:`, error));
    }, []);

    const refreshDeviceDetails = useCallback((mac, includeRaw = true, batchId) => {
        const monitorVersion = monitorSwitches.current.version();
        const eventVersion = deviceEventVersions.current.get(mac);
        const deviceRequest = fetch(
            `http://${window.location.hostname}:8080/api/device/list`
        )
            .then(async response => {
                if (!response.ok)
                {
                    throw new Error(`HTTP ${response.status}`);
                }
                return response.json();
            })
            .then(data => {
                const observed = Array.isArray(data)
                    ? data.find(device => device.mac === mac)
                    : null;
                if (!observed || deviceEventVersions.current.get(mac) !== eventVersion) return;
                const current = monitorSwitches.current.snapshot(observed, monitorVersion);

                const normalized = {
                    ...current,
                    model_name: current.model_name || 'Unknown Device',
                    custom_name: current.custom_name || '',
                    name: current.name || buildDefaultDeviceName(
                        current.model_name,
                        current.mac
                    ),
                    work_mode: current.work_mode || 'sleep'
                };

                setWhitelist(previous => previous.map(device => device.mac === mac
                    ? mergeDeviceSnapshot(device, normalized, formatCollectionTime)
                    : device));
                setNodeMonitoring(previous => ({
                    ...previous,
                    [mac]: Boolean(current.online_monitoring_enabled)
                }));
                setCollectionStatus(previous => ({
                    ...previous,
                    [mac]: collectionStateText(current.collection_state)
                }));
            })
            .catch(error => {
                console.error(`刷新节点 ${mac} 详情失败:`, error);
            });

        return deviceRequest.then(() => loadLatestVibration(mac, includeRaw, () => true, batchId));
    }, [loadLatestVibration]);



    const samplingConfigToForm = (config, device) => ({
        id: device.mac, name: device.name, isDefault: false,
        automatic_interval_minutes: config.auto_interval_sec / 60,
        vibration_sampling_rate: config.vibration_rate_hz,
        digital_microphone_sampling_rate: config.digital_mic_rate_hz,
        analog_microphone_sampling_rate: config.analog_mic_rate_hz,
        vibration_window_seconds: config.vibration_window_ms / 1000,
        digital_microphone_window_seconds: config.digital_mic_window_ms / 1000,
        analog_microphone_window_seconds: config.analog_mic_window_ms / 1000,
        raw_upload_every_cycles: config.raw_upload_every_cycles,
        vibration_enabled: config.vibration_enabled,
        digital_microphone_enabled: config.digital_microphone_enabled,
        analog_microphone_enabled: config.analog_microphone_enabled
    });

    const openNodeSampling = async device => {
        const request = ++samplingRequest.current;
        setSamplingNode({ ...device });
        setSamplingDraft(null);
        setSamplingNotice('');
        setSamplingLoading(true);
        setSamplingSaving(false);
        setActiveTab('node-settings');
        try {
            const query = new URLSearchParams({ mac: device.mac });
            const response = await fetch(`http://${window.location.hostname}:8080/api/device/sampling?${query}`);
            const data = await response.json();
            if (!response.ok || data.status !== 'success' || data.mac !== device.mac || !data.config)
                throw new Error(data.msg || '无法读取节点采样配置');
            if (samplingRequest.current !== request) return;
            setSamplingDraft(samplingConfigToForm(data.config, device));
        } catch (error) {
            if (samplingRequest.current === request) setSamplingNotice(`读取失败：${error.message}，请返回重试。`);
        } finally {
            if (samplingRequest.current === request) setSamplingLoading(false);
        }
    };
    const closeNodeSampling = () => {
        if (samplingSaving) return;
        ++samplingRequest.current;
        setActiveTab('devices');
    };
    const updateNodeSampling = (_id, field, value) => {
        setSamplingDraft(previous => ({ ...previous, [field]: value }));
        setSamplingNotice('');
    };
    const saveNodeSampling = async () => {
        if (!samplingNode || !samplingDraft || samplingSaving || samplingLoading) return;
        const request = samplingRequest.current;
        const device = samplingNode;
        const draft = samplingDraft;
        setSamplingSaving(true);
        setSamplingNotice('');
        try {
            const response = await fetch(`http://${window.location.hostname}:8080/api/device/sampling`, {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    mac: device.mac,
                    auto_interval_sec: Math.round(draft.automatic_interval_minutes * 60),
                    vibration_window_ms: Math.round(draft.vibration_window_seconds * 1000),
                    digital_mic_window_ms: Math.round(draft.digital_microphone_window_seconds * 1000),
                    analog_mic_window_ms: Math.round(draft.analog_microphone_window_seconds * 1000),
                    raw_upload_every_cycles: draft.raw_upload_every_cycles,
                    vibration_enabled: draft.vibration_enabled,
                    digital_microphone_enabled: draft.digital_microphone_enabled,
                    analog_microphone_enabled: draft.analog_microphone_enabled
                })
            });
            const data = await response.json();
            if (!response.ok || data.status !== 'success' || data.mac !== device.mac || !data.config)
                throw new Error(data.msg || '保存失败');
            if (samplingRequest.current !== request) return;
            setSamplingDraft(samplingConfigToForm(data.config, device));
            setSamplingNotice('已保存到网关；等待节点唤醒、上传结束或排队时同步。当前采集不打断，新参数从后续采集周期生效。');
        } catch (error) {
            if (samplingRequest.current === request) setSamplingNotice(`保存失败：${error.message}`);
        } finally {
            if (samplingRequest.current === request) setSamplingSaving(false);
        }
    };

    const applyChannelOperationResult = useCallback((state, currentChannel, message,
        targetChannel = null, preparedNodes = null, totalNodes = null) => {
        const actualChannel = Number(currentChannel);
        const hasActualChannel = Number.isInteger(actualChannel) &&
            actualChannel >= 11 && actualChannel <= 26;
        const requestedChannel = Number(targetChannel);
        const isManualOperation = Number.isInteger(requestedChannel) &&
            requestedChannel >= 11 && requestedChannel <= 26;

        setSystemSettings(previous => ({
            ...previous,
            ...(state === 'success' && hasActualChannel && isManualOperation
                ? {
                    channel_strategy: 'manual',
                    target_channel: actualChannel,
                    current_channel: actualChannel
                }
                : hasActualChannel ? { current_channel: actualChannel } : {}),
            channel_operation: {
                ...(previous.channel_operation || {}),
                state,
                target_channel: isManualOperation
                    ? requestedChannel
                    : (targetChannel == null ? null : requestedChannel),
                current_channel: hasActualChannel ? actualChannel : null,
                message: message || '',
                prepared_nodes: preparedNodes == null
                    ? previous.channel_operation?.prepared_nodes
                    : Number(preparedNodes),
                total_nodes: totalNodes == null
                    ? previous.channel_operation?.total_nodes
                    : Number(totalNodes)
            }
        }));

        setChannelSaveNotice(state === 'pending'
            ? '信道操作等待执行，可在“协调器状态”查看进度。'
            : state === 'success'
                ? '最近信道操作执行成功。'
                : '最近信道操作执行失败，请查看协调器状态。');

        if (state === 'success' && hasActualChannel && isManualOperation && !channelDraftEdited.current)
            setChannelDraft(String(actualChannel));
    }, []);

    const saveChannelStrategy = async () => {
        const manual = channelDraft !== 'auto';
        const requestedChannel = manual ? Number(channelDraft) : null;
        const savedDraft = systemSettings.channel_strategy === 'manual'
            ? String(systemSettings.target_channel)
            : 'auto';
        const confirmMessage = manual
            ? whitelist.length > 0
                ? `当前网络中有 ${whitelist.length} 个节点。确认提交切换至信道 ${requestedChannel} 的全网迁移操作吗？`
                : `当前网络中没有节点。确认让协调器切换至信道 ${requestedChannel} 吗？`
            : '确认将信道配置设置为自动选择吗？当前网络仍保持实际工作信道。';
        if (!window.confirm(confirmMessage))
        {
            channelDraftEdited.current = false;
            setChannelDraft(savedDraft);
            return;
        }

        setChannelAction(true);
        setChannelSaveNotice('正在提交信道操作…');
        try
        {
            const response = await fetch(`http://${window.location.hostname}:8080/api/system/channel`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    channel_strategy: manual ? 'manual' : 'auto',
                    target_channel: manual ? Number(channelDraft) : null
                })
            });
            const data = await response.json();
            if (!response.ok || data.status !== 'accepted')
                throw new Error(data.msg || `HTTP ${response.status}`);
            channelDraftEdited.current = false;
            applyChannelOperationResult(
                'pending',
                systemSettings.current_channel,
                data.msg || '信道操作已提交，等待执行',
                requestedChannel,
                0,
                Number(data.device_count) || 0
            );
            setChannelSaveNotice('信道操作已提交，可在“协调器状态”查看结果。');
        }
        catch (error)
        {
            channelDraftEdited.current = false;
            applyChannelOperationResult(
                'failed',
                systemSettings.current_channel,
                `信道配置提交失败：${error.message}`,
                requestedChannel
            );
            setChannelDraft(savedDraft);
            setChannelSaveNotice(`提交失败：${error.message}`);
            alert(`信道配置提交失败：${error.message}`);
        }
        finally
        {
            setChannelAction(false);
        }
    };


    /* 同步协调器当前网络信息与已保存的信道配置。 */
    const loadSystemSettings = useCallback(() => {
        setSettingsLoading(true);

        return fetch(
            `http://${window.location.hostname}:8080/api/system/settings`
        )
            .then(response => {
                if (!response.ok)
                {
                    throw new Error(`HTTP ${response.status}`);
                }

                return response.json();
            })
            .then(data => {
                setSystemSettings(data);
                if (!channelDraftEdited.current) setChannelDraft(
                    data.channel_operation?.state === 'pending' &&
                    Number(data.channel_operation?.target_channel) >= 11 &&
                    Number(data.channel_operation?.target_channel) <= 26
                        ? String(data.channel_operation.target_channel)
                        : data.channel_strategy === 'manual'
                            ? String(data.target_channel)
                            : 'auto'
                );
                setGatewayStatus(previous => ({
                    ...previous,
                    apiConnected: true,
                    lastApiCheck: Date.now()
                }));
            })
            .catch(error => {
                console.error('加载系统设置失败:', error);
                setGatewayStatus(previous => ({
                    ...previous,
                    apiConnected: false,
                    lastApiCheck: Date.now()
                }));
            })
            .finally(() => {
                setSettingsLoading(false);
            });
    }, []);


    const checkGatewayClock = useCallback(() => {
        const requestStartedAt = Date.now();
        return fetch(
            `http://${window.location.hostname}:8080/api/system/time`,
            {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({ client_time_ms: requestStartedAt })
            }
        )
            .then(async response => {
                const data = await response.json();
                if (!response.ok || data.status !== 'success')
                {
                    const error = new Error(data.msg || `HTTP ${response.status}`);
                    error.backendReachable = true;
                    throw error;
                }

                const responseReceivedAt = Date.now();
                const clientMidpoint = requestStartedAt +
                    (responseReceivedAt - requestStartedAt) / 2;
                const serverTimeMs = Number(data.server_time_ms);
                const timeOffsetMs = Number.isFinite(serverTimeMs)
                    ? Math.round(serverTimeMs - clientMidpoint)
                    : null;
                const timeAligned = timeOffsetMs !== null &&
                    Math.abs(timeOffsetMs) <= 2000;

                console.log(
                    data.corrected
                        ? '🕒 RK网关时间已按浏览器校正'
                        : '🕒 RK网关时间已同步'
                );
                setGatewayStatus(previous => ({
                    ...previous,
                    apiConnected: true,
                    lastApiCheck: responseReceivedAt,
                    serverTimeMs: Number.isFinite(serverTimeMs) ? serverTimeMs : null,
                    serverTimeCapturedAt: Number.isFinite(serverTimeMs)
                        ? responseReceivedAt
                        : null,
                    timeAligned,
                    timeOffsetMs,
                    timeLastCheck: responseReceivedAt,
                    timeSyncError: timeAligned ? '' : '时间偏差超过2秒'
                }));
                return data;
            })
            .catch(error => {
                console.error('同步RK网关时间失败:', error);
                setGatewayStatus(previous => ({
                    ...previous,
                    apiConnected: error.backendReachable ? true : false,
                    lastApiCheck: Date.now(),
                    timeAligned: false,
                    timeOffsetMs: null,
                    timeLastCheck: Date.now(),
                    timeSyncError: error.message
                }));
                return null;
            });
    }, []);


    const loadSystemResources = useCallback(() => {
        return fetch(
            `http://${window.location.hostname}:8080/api/system/resources`
        )
            .then(async response => {
                const data = await response.json();
                if (!response.ok || data.status !== 'success')
                {
                    throw new Error(data.msg || `HTTP ${response.status}`);
                }
                return data;
            })
            .then(data => {
                setSystemResources({
                    data,
                    lastUpdated: Date.now(),
                    error: ''
                });
                return data;
            })
            .catch(error => {
                console.error('读取Linux网关系统资源失败:', error);
                // 接口不可用时清空旧值，避免把历史采样冒充实时状态。
                setSystemResources({
                    data: null,
                    lastUpdated: null,
                    error: error.message
                });
                return null;
            });
    }, []);


    /* 网关时间以最近一次后端返回值为基准逐秒显示，并定期重新校准。
     * 浏览器后台定时器即使被节流，恢复后也会按 Date.now() 自动追上。 */
    useEffect(() => {
        const clockTimer = window.setInterval(() => {
            setGatewayClockTick(Date.now());
        }, 1000);
        const calibrationTimer = window.setInterval(() => {
            checkGatewayClock();
        }, 30000);

        return () => {
            window.clearInterval(clockTimer);
            window.clearInterval(calibrationTimer);
        };
    }, [checkGatewayClock]);


    /* 系统资源只在网关状态页读取，每5秒更新一次。 */
    useEffect(() => {
        if (activeTab !== 'gateway-status') return undefined;

        loadSystemResources();
        const timer = window.setInterval(loadSystemResources, 5000);
        return () => window.clearInterval(timer);
    }, [activeTab, loadSystemResources]);


    /* Status/settings pages refresh saved backend state every five seconds.
     * This read-only polling must not overwrite an unsaved channel selection. */
    useEffect(() => {
        if (!['gateway-status', 'coordinator-status', 'settings'].includes(activeTab)) return undefined;
        const initial = window.setTimeout(loadSystemSettings, 0);
        const timer = window.setInterval(loadSystemSettings, 5000);
        return () => { window.clearTimeout(initial); window.clearInterval(timer); };
    }, [activeTab, loadSystemSettings]);


    const loadMonitoringSettings = useCallback(() => {
        return fetch(`http://${window.location.hostname}:8080/api/monitor/policies`)
            .then(async response => {
                const data = await response.json();
                if (!response.ok || data.status !== 'success')
                    throw new Error(data.msg || `HTTP ${response.status}`);
                if (!Array.isArray(data.policies)) throw new Error('配置列表格式无效');
                const policies = data.policies.map(item => ({
                    id: item.is_default ? DEFAULT_MONITOR_POLICY_ID : String(item.id),
                    name: item.name || (item.is_default ? '默认采样配置' : '未命名配置'),
                    isDefault: Boolean(item.is_default),
                    automatic_interval_minutes: Number(item.auto_interval_sec) / 60,
                    vibration_sampling_rate: Number(item.vibration_rate_hz),
                    digital_microphone_sampling_rate: Number(item.digital_mic_rate_hz),
                    analog_microphone_sampling_rate: Number(item.analog_mic_rate_hz),
                    vibration_window_seconds: Number(item.vibration_window_ms) / 1000,
                    digital_microphone_window_seconds: Number(item.digital_mic_window_ms) / 1000,
                    analog_microphone_window_seconds: Number(item.analog_mic_window_ms) / 1000,
                    raw_upload_every_cycles: Number(item.raw_upload_every_cycles) || 1,
                    vibration_enabled: item.vibration_enabled !== false,
                    digital_microphone_enabled: item.digital_microphone_enabled !== false,
                    analog_microphone_enabled: item.analog_microphone_enabled !== false
                }));
                if (!policies.some(policy => policy.isDefault))
                    throw new Error('后端未返回默认采样配置');
                setMonitorPolicies(policies);
            })
            .catch(error => {
                console.error('加载节点采样配置失败:', error);
            });
    }, []);


    /* 浏览器尺寸 */
    useEffect(() => {
        const handleResize = () => {
            setWindowWidth(window.innerWidth);
        };

        window.addEventListener('resize', handleResize);

        return () => {
            window.removeEventListener('resize', handleResize);
        };
    }, []);


    /* 即使节点暂停上报，最近30分钟时间窗也继续按真实时间向前推进。 */
    useEffect(() => {
        const timer = window.setInterval(() => {
            setTrendWindowEnd(Date.now());
        }, 1000);

        return () => {
            window.clearInterval(timer);
        };
    }, []);


    /* 无RTC网关先采用浏览器时间授时，成功或失败后再初始化节点状态。 */
    useEffect(() => {
        let cancelled = false;

        const syncGatewayTime = checkGatewayClock();
        let refreshTimer, refreshing = false;
        // Poll details as well as the device list: WebSocket notifications may be missed.
        const refresh = () => {
            if (refreshing || cancelled) return;
            refreshing = true;
            if (cancelled) return;
            const monitorVersion = monitorSwitches.current.version();
            const eventsAtRequest = new Map(deviceEventVersions.current);
            fetch(`http://${window.location.hostname}:8080/api/device/list`)
            .then(async response => {
                if (!response.ok) throw new Error(`HTTP ${response.status}`);
                const data = await response.json();
                if (!Array.isArray(data)) throw new Error('节点状态格式无效');
                return data;
            })
            .then(data => {
                if (cancelled) return;

                const safeData = data.map(observed => {
                    const dev = monitorSwitches.current.snapshot(observed, monitorVersion);
                    const isOffline =
                        (dev.status || '').toLowerCase() !== 'online';

                    const currentTemp = parseFloat(dev.temp);

                    return {
                        ...dev,
                        model_name: dev.model_name || 'Unknown Device',
                        custom_name: dev.custom_name || '',
                        name: dev.name || buildDefaultDeviceName(
                            dev.model_name,
                            dev.mac
                        ),
                        temp: dev.temp,
                        mag: dev.mag,
                        digital_db: dev.digital_db ?? '--',
                        analog_db: dev.analog_db ?? '--',
                        is_alarm:
                            !isOffline &&
                            !isNaN(currentTemp) &&
                            currentTemp > TEMP_LIMIT,
                        work_mode: dev.work_mode || 'sleep',
                        last_sample_time: formatCollectionTime(dev.last_collection)
                    };
                });


                setWhitelist(previous => {
                    const byMac = new Map(previous.map(device => [device.mac, device]));
                    return safeData.map(device => byMac.has(device.mac) &&
                        deviceEventVersions.current.get(device.mac) !== eventsAtRequest.get(device.mac)
                        ? byMac.get(device.mac) : mergeDeviceSnapshot(byMac.get(device.mac), device, formatCollectionTime));
                });
                setNodeMonitoring(safeData.reduce((result, device) => ({
                    ...result,
                    [device.mac]: Boolean(device.online_monitoring_enabled)
                }), {}));
                setCollectionStatus(safeData.reduce((result, device) => ({
                    ...result,
                    [device.mac]: collectionStateText(device.collection_state)
                }), {}));
                return Promise.all(safeData.filter(device =>
                    deviceEventVersions.current.get(device.mac) === eventsAtRequest.get(device.mac)).map(device => {
                    const batch = device.collection_state === 'complete' && device.capture_batch_id &&
                        device.capture_batch_id !== device.collection_batch_id ? device.capture_batch_id : device.collection_batch_id;
                    return loadLatestVibration(device.mac, false, () => !cancelled, batch);
                }));
            })
            .catch(error => {
                console.error("同步节点状态失败:", error);
                if (cancelled) return;
                // 后端不可用时必须展示真实的“无数据”状态，
                // 不保留热更新会话中的旧节点，也不填充演示节点。
                setWhitelist([]);
                setNodeMonitoring({});
                setCollectionStatus({});
            }).finally(() => { refreshing = false; });
        };
        syncGatewayTime.finally(() => {
            if (cancelled) return;
            refresh(); refreshTimer = window.setInterval(refresh, 3000);
        });
        return () => {
            cancelled = true; window.clearInterval(refreshTimer);
        };
    }, [checkGatewayClock, loadLatestVibration, admissionRevision]);


    /* WebSocket */
    useEffect(() => {
        let ws;
        let reconnectTimer;
        let stopped = false;

        const connectWS = () => {
            if (stopped || (ws && [WebSocket.CONNECTING, WebSocket.OPEN].includes(ws.readyState))) return;
            const socket = new WebSocket(
                `ws://${window.location.hostname}:8080/api/ws`
            );
            ws = socket;

            socket.onopen = () => {
                if (stopped || ws !== socket) return;
                console.log('🟢 WebSocket 实时通信通道已打通');
                setGatewayStatus(previous => ({
                    ...previous,
                    wsConnected: true
                }));
            };

            socket.onmessage = (event) => {
                if (stopped || ws !== socket) return;
                try
                {
                    const incoming = JSON.parse(event.data);
                    if (incoming.mac && ['temp_update', 'mag_update', 'sound_update', 'monitor_update',
                        'collection_data_ready', 'vibration_upload_progress'].includes(incoming.type)) {
                        // Discard HTTP responses captured before this newer live event.
                        vibrationRequests.current.delete(incoming.mac);
                        deviceEventVersions.current.set(incoming.mac, (deviceEventVersions.current.get(incoming.mac) || 0) + 1);
                    }
                    recordHomeActivity(incoming);
                    if (incoming.mac && (['temp_update', 'mag_update', 'sound_update', 'collection_data_ready', 'vibration_upload_progress'].includes(incoming.type) ||
                        (incoming.type === 'monitor_update' && Number(incoming.link_updated_at) > 0))) {
                        const seen = Number(incoming.link_updated_at) > 0 ? Number(incoming.link_updated_at) * 1000 : Date.now();
                        setNodeLastUplink(previous => ({ ...previous, [incoming.mac]: Math.max(previous[incoming.mac] || 0, seen) }));
                    }

                    console.log('📡 收到底层网关推送:', incoming);

                    if (incoming.type === 'status_update')
                    {
                        setWhitelist(prev => prev.map(dev => {
                            if (dev.mac !== incoming.mac)
                            {
                                return dev;
                            }

                            const isOffline =
                                incoming.status.toLowerCase() !== 'online';

                            return {
                                ...dev,
                                status: incoming.status,
                                short_addr: incoming.short_addr ?? dev.short_addr,
                                last_active: incoming.last_active ?? dev.last_active,
                                is_alarm: isOffline ? false : dev.is_alarm
                            };
                        }));
                    }
                    else if (incoming.type === 'description_update') {
                        setWhitelist(prev => prev.map(dev => dev.mac === incoming.mac ? { ...dev, description: incoming.description } : dev));
                    }
                    else if (incoming.type === 'temp_update')
                    {
                        const currentTemp = Number(incoming.temp);
                        const isAlarm = currentTemp > TEMP_LIMIT;
                        setTrendWindowEnd(Date.now());

                        setWhitelist(prev => prev.map(dev => {
                            if (dev.mac !== incoming.mac)
                            {
                                return dev;
                            }

                            if (isAlarm && !dev.is_alarm)
                            {
                                console.error(
                                    `🔥 警报：节点 ${dev.name || dev.mac} 温度超限！当前温度: ${currentTemp} ℃`
                                );
                            }

                            return mergeLiveCollection(dev, incoming);
                        }));

                        setTempHistory(prev => {
                            const now = Date.now();
                            const minTime = now - TEMP_HISTORY_TIME;

                            const history = [
                                ...(prev[incoming.mac] || []),
                                {
                                    time: now,
                                    temp: currentTemp
                                }
                            ].filter(item => item.time >= minTime);

                            return {
                                ...prev,
                                [incoming.mac]: history
                            };
                        });
                    }
                    else if (incoming.type === 'mag_update')
                    {
                        setWhitelist(prev => prev.map(dev => {
                            if (dev.mac !== incoming.mac)
                            {
                                return dev;
                            }

                            return mergeLiveCollection(dev, incoming);
                        }));
                    }
                    else if (incoming.type === 'sound_update')
                    {
                        const currentDb = Number(incoming.db);
                        const source = incoming.source;
                        const serverTimestamp = Number(incoming.timestamp);
                        const serverEventTime = serverTimestamp * 1000;
                        const eventTime =
                            incoming.timestamp !== undefined &&
                            incoming.timestamp !== null &&
                            Number.isFinite(serverEventTime) &&
                            serverEventTime >= MIN_VALID_GATEWAY_TIME_MS
                            ? serverEventTime
                            : Date.now();

                        if (
                            !Number.isFinite(currentDb) ||
                            (source !== 'digital' && source !== 'analog')
                        )
                        {
                            console.error('收到无效麦克风数据:', incoming);
                            return;
                        }

                        setTrendWindowEnd(previous => Math.max(previous, eventTime));

                        setWhitelist(prev => prev.map(dev => {
                            if (dev.mac !== incoming.mac)
                            {
                                return dev;
                            }

                            return mergeLiveCollection(dev, incoming);
                        }));

                        setSoundHistory(prev => {
                            const minTime = Date.now() - TEMP_HISTORY_TIME;
                            const deviceHistory = prev[incoming.mac] || {
                                digital: [],
                                analog: []
                            };
                            const historyByTime = new Map();

                            (deviceHistory[source] || [])
                                .filter(item => item.time >= minTime)
                                .forEach(item => historyByTime.set(item.time, item));

                            historyByTime.set(eventTime, {
                                time: eventTime,
                                db: currentDb
                            });

                            return {
                                ...prev,
                                [incoming.mac]: {
                                    ...deviceHistory,
                                    [source]: Array
                                        .from(historyByTime.values())
                                        .sort((a, b) => a.time - b.time)
                                }
                            };
                        });
                    }
                    else if (incoming.type === 'monitor_setting')
                    {
                        setWhitelist(previous => previous.map(node => node.mac === incoming.mac
                            ? { ...node, online_monitoring_enabled: Boolean(incoming.enabled) } : node));
                    }
                    else if (incoming.type === 'monitor_update')
                    {
                        setWhitelist(previous => previous.map(device => device.mac === incoming.mac
                            ? mergeLiveCollection(device, incoming) : device));
                        if (Number.isInteger(incoming.lqi) && incoming.lqi >= 0 && incoming.lqi <= 255) {
                            setWhitelist(previous => previous.map(device => device.mac === incoming.mac
                                ? { ...device, status: 'Online', lqi: incoming.lqi, rssi: incoming.rssi,
                                    power_source: incoming.power_source, link_updated_at: incoming.link_updated_at }
                                : device));
                        }
                        setNodeMonitoring(previous => ({
                            ...previous,
                            [incoming.mac]: Boolean(incoming.monitoring_enabled)
                        }));
                        setCollectionStatus(previous => ({
                            ...previous,
                            [incoming.mac]: collectionStateText(incoming.collection_state)
                        }));
                        /* 卡片始终展示；完成时刷新特征和上传状态，完整波形按按钮加载。 */
                        if (incoming.collection_state === 'complete' || incoming.collection_state === 'uploading')
                        {
                            refreshDeviceDetails(incoming.mac, false, incoming.batch_id);
                        }
                    }
                    else if (incoming.type === 'collection_data_ready')
                    {
                        setWhitelist(previous => previous.map(device => device.mac === incoming.mac
                            ? mergeLiveCollection(device, incoming) : device));
                        setCollectionStatus(previous => ({
                            ...previous,
                            [incoming.mac]: incoming.raw_complete
                                ? '原始波形已接收'
                                : '振动数据已接收'
                        }));

                        refreshDeviceDetails(incoming.mac, false, incoming.batch_id);
                    }
                    else if (incoming.type === 'vibration_upload_progress')
                    {
                        setWhitelist(previous => previous.map(device => device.mac === incoming.mac
                            ? mergeLiveCollection(device, incoming) : device));
                        if (incoming.status === 'queued')
                            setCollectionStatus(previous => ({ ...previous, [incoming.mac]: '等待数据上传' }));
                        if (incoming.status !== 'queued' && Number(incoming.received_packets) > 0)
                            loadLatestVibration(incoming.mac, false, () => !stopped, incoming.batch_id);
                    }
                    else if (incoming.type === 'channel_migration')
                    {
                        applyChannelOperationResult(
                            incoming.state,
                            incoming.current_channel,
                            incoming.message,
                            incoming.target_channel,
                            incoming.prepared_nodes,
                            incoming.total_nodes
                        );
                    }
                    else if (incoming.type === 'name_update')
                    {
                        setWhitelist(prev => prev.map(dev => {
                            if (dev.mac !== incoming.mac)
                            {
                                return dev;
                            }

                            return {
                                ...dev,
                                name: incoming.name || dev.name,
                                model_name:
                                    incoming.model_name || dev.model_name,
                                custom_name:
                                    incoming.custom_name ?? dev.custom_name
                            };
                        }));
                    }

                    /*
                     * 后面增加真实工作模式时：
                     *
                     * else if (incoming.type === 'work_mode_update')
                     * {
                     *     ...
                     * }
                     */
                }
                catch (err)
                {
                    console.error("数据解析失败", err);
                }
            };

            socket.onclose = () => {
                if (stopped || ws !== socket) return;
                ws = null;
                setJoinState(previous => ({ ...previous, known: false }));
                console.log(
                    '🔴 WebSocket 断开，3秒后尝试自动重连...'
                );

                setGatewayStatus(previous => ({
                    ...previous,
                    wsConnected: false
                }));

                clearTimeout(reconnectTimer);
                reconnectTimer = setTimeout(() => {
                    reconnectTimer = undefined;
                    connectWS();
                }, 3000);
            };

            socket.onerror = (err) => {
                if (stopped || ws !== socket) return;
                console.error('⚠️ WebSocket 发生错误:', err);
                socket.close();
            };
        };

        connectWS();

        return () => {
            // Closing a socket is asynchronous. Stop and detach its callbacks
            // first so StrictMode/effect cleanup cannot resurrect this session.
            stopped = true;
            clearTimeout(reconnectTimer);

            if (ws)
            {
                const socket = ws;
                ws = null;
                socket.onopen = socket.onmessage = socket.onclose = socket.onerror = null;
                socket.close();
            }
        };
    }, [applyChannelOperationResult, refreshDeviceDetails, recordHomeActivity, loadLatestVibration]);


    useEffect(() => {
        let cancelled = false, timer;
        const controller = new AbortController();
        const poll = async () => {
            try {
                const response = await fetch(`http://${window.location.hostname}:8080/api/device/scan_status`, { signal: controller.signal });
                if (!response.ok) throw new Error('无法读取协调器状态');
                const state = await response.json();
                if (!cancelled) {
                    setJoinState(state);
                    setJoinIntent(previous => previous !== null && state.active === previous && !state.pending ? null : previous);
                }
            } catch (error) {
                if (!cancelled) setJoinState(previous => ({ ...previous, known: false, error: error.message }));
            } finally {
                if (!cancelled) timer = window.setTimeout(poll, 1000);
            }
        };
        poll();
        return () => { cancelled = true; controller.abort(); window.clearTimeout(timer); };
    }, []);
    const requestJoin = async active => {
        setJoinIntent(active);
        setJoinState(previous => ({ ...previous, pending: true, error: '' }));
        try {
            const response = await fetch(`http://${window.location.hostname}:8080/api/device/${active ? 'scan' : 'scan_stop'}`, { method: 'POST' });
            const result = await response.json();
            if (!response.ok || result.status !== 'success') throw new Error(result.msg || '入网请求失败');
        } catch (error) {
            setJoinIntent(null);
            setJoinState(previous => ({ ...previous, pending: false, error: error.message }));
            alert(error.message);
        }
    };
    const handleStartScan = () => requestJoin(true);
    const handleStopScan = () => requestJoin(false);
    const retryInterview = async mac => {
        try {
            const response = await fetch(`http://${window.location.hostname}:8080/api/device/interview`, {
                method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ mac })
            });
            if (!response.ok) throw new Error('无法提交重新识别，请检查节点及协调器');
        } catch (error) { alert(error.message); }
    };

    /* 手动添加 */
    const handleManualAdd = () => {
        const cleanMac = manualMac.trim();
        const macRegex = /^([0-9A-Fa-f]{2}:){7}[0-9A-Fa-f]{2}$/;

        if (!macRegex.test(cleanMac))
        {
            alert("MAC 地址格式不正确！");
            return;
        }

        const normalizedMac = cleanMac.toUpperCase();

        fetch(
            `http://${window.location.hostname}:8080/api/device/add`,
            {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({
                    mac: normalizedMac
                })
            }
        )
            .then(response => response.json())
            .then(data => {
                if (data.status === 'success')
                {
                    /* 相同 MAC 可能刚被删除后重新加入，必须覆盖旧的前端缓存。 */
                    setNodeMonitoring(previous => ({
                        ...previous,
                        [normalizedMac]: false
                    }));
                    setCollectionStatus(previous => ({
                        ...previous,
                        [normalizedMac]: ''
                    }));
                    // Registration is not join completion. The server list
                    // adds the node only after a valid post-join uplink.
                    setWhitelist(prev => prev.filter(dev => dev.mac !== normalizedMac));

                    setManualMac('');

                    alert(
                        "MAC 已登记，节点完成入网后会自动显示在列表中。"
                    );
                }
                else if (data.status === 'exists')
                {
                    alert(
                        "该 MAC 已登记；尚未显示的节点需等待完成入网。"
                    );
                }
                else
                {
                    alert("网关报错：" + data.msg);
                }
            })
            .catch(error => {
                console.error("网络请求失败:", error);
            });
    };


    const applyNameResponse = (mac, data) => {
        setWhitelist(prev => prev.map(dev => {
            if (dev.mac !== mac)
            {
                return dev;
            }

            return {
                ...dev,
                name: data.name,
                model_name: data.model_name || dev.model_name,
                custom_name: data.custom_name || ''
            };
        }));
    };


    const requestDeviceRename = (mac, customName) => {
        setSavingNameMac(mac);

        return fetch(
            `http://${window.location.hostname}:8080/api/device/rename`,
            {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({
                    mac,
                    custom_name: customName
                })
            }
        )
            .then(response => response.json())
            .then(data => {
                if (data.status !== 'success')
                {
                    if (data.status === 'duplicate')
                    {
                        throw new Error('该节点名称已被其他设备使用');
                    }

                    throw new Error(data.msg || '修改名称失败');
                }

                applyNameResponse(mac, data);
                return data;
            })
            .finally(() => {
                setSavingNameMac(null);
            });
    };


    const requestDeviceDescription = async (mac, description) => {
        const response = await fetch(`http://${window.location.hostname}:8080/api/device/description`, {
            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ mac, description })
        });
        const data = await response.json();
        if (!response.ok || data.status !== 'success') throw new Error(data.msg || '保存描述失败');
        setWhitelist(prev => prev.map(dev => dev.mac === mac ? { ...dev, description: data.description } : dev));
        return data;
    };

    const startRenameDevice = (dev) => {
        const next = { ...nodeColumns, name: true, actions: true };
        setNodeColumns(next);
        try { localStorage.setItem(COLUMN_STORAGE_KEY, JSON.stringify(next)); } catch { /* Keep session preference. */ }
        setRenamingMac(dev.mac);
        setRenameDraft(dev.custom_name || '');
    };


    const cancelRenameDevice = () => {
        setRenamingMac(null);
        setRenameDraft('');
    };


    const saveDeviceName = (dev) => {
        const cleanName = renameDraft.trim();

        if (!cleanName)
        {
            alert('节点名称不能为空；如需使用自动名称，请点击“恢复默认”。');
            return;
        }

        if (Array.from(cleanName).length > 32)
        {
            alert('节点名称最多允许 32 个字符。');
            return;
        }

        const hasDuplicate = whitelist.some(item =>
            item.mac !== dev.mac &&
            String(item.name || '').trim().toLocaleLowerCase() ===
                cleanName.toLocaleLowerCase()
        );

        if (hasDuplicate)
        {
            alert('该节点名称已被其他设备使用，请换一个名称。');
            return;
        }

        requestDeviceRename(dev.mac, cleanName)
            .then(() => {
                cancelRenameDevice();
            })
            .catch(error => {
                alert(`重命名失败：${error.message}`);
            });
    };


    const restoreDefaultDeviceName = (dev) => {
        if (!window.confirm(`确定恢复节点 ${dev.name} 的默认名称吗？`))
        {
            return;
        }

        requestDeviceRename(dev.mac, '')
            .then(() => {
                if (renamingMac === dev.mac)
                {
                    cancelRenameDevice();
                }
            })
            .catch(error => {
                alert(`恢复默认名称失败：${error.message}`);
            });
    };


    /* 删除节点 */
    const handleRemoveDevice = (mac) => {
        if (
            !window.confirm(
                `确定要彻底移除节点 ${mac} 吗？\n这将会从数据库删除并强制它退出网络！`
            )
        )
        {
            return;
        }

        fetch(
            `http://${window.location.hostname}:8080/api/device/remove`,
            {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({
                    mac
                })
            }
        )
            .then(response => response.json())
            .then(data => {
                if (data.status === 'success')
                {
                    setWhitelist(prev =>
                        prev.filter(dev => dev.mac !== mac)
                    );


                    setTempHistory(prev => {
                        const next = { ...prev };
                        delete next[mac];
                        return next;
                    });

                    setSoundHistory(prev => {
                        const next = { ...prev };
                        delete next[mac];
                        return next;
                    });

                    setNodeMonitoring(prev => {
                        const next = { ...prev };
                        delete next[mac];
                        return next;
                    });

                    setCollectionStatus(prev => {
                        const next = { ...prev };
                        delete next[mac];
                        return next;
                    });

                    alert("节点已彻底移除并踢出网络！");
                }
                else
                {
                    alert("移除失败：" + data.msg);
                }
            })
            .catch(error => {
                console.error("移除节点网络请求失败:", error);
            });
    };


    /* 加载节点最近一段时间的温度历史 */


    /* 加载节点最近一段时间的两路麦克风历史 */




    const overviewColumns =
        windowWidth >= 1100
            ? 'repeat(2, minmax(0, 1fr))'
            : '1fr';
    const statusColumns = windowWidth >= 1180
        ? 'repeat(3, minmax(0, 1fr))'
        : windowWidth >= 760
            ? 'repeat(2, minmax(0, 1fr))'
            : '1fr';
    const onlineNodeCount = whitelist.filter(device =>
        String(device.status || '').toLowerCase() === 'online'
    ).length;
    const offlineNodeCount = whitelist.filter(device =>
        String(device.status || '').toLowerCase() === 'offline'
    ).length;
    const gatewayClockAvailable = Number.isFinite(gatewayStatus.serverTimeMs) &&
        Number.isFinite(gatewayStatus.serverTimeCapturedAt);
    const gatewayClockNowMs = gatewayClockAvailable
        ? gatewayStatus.serverTimeMs +
            Math.max(0, gatewayClockTick - gatewayStatus.serverTimeCapturedAt)
        : null;
    const gatewayClockStale = gatewayClockAvailable &&
        gatewayClockTick - gatewayStatus.serverTimeCapturedAt > 90000;
    const coordinatorOnline = gatewayStatus.apiConnected === true &&
        systemSettings.coordinator_online === true;
    const zigbeeNetworkReady = coordinatorOnline &&
        systemSettings.network_state === 'ready';
    const networkSourceText = systemSettings.network_source === 'restored'
        ? '旧网络恢复'
        : systemSettings.network_source === 'auto_scan'
            ? '自动扫描建网'
            : systemSettings.network_source === 'realtime'
                ? '协调器实时查询'
                : '等待协调器上报';
    const recentChannelOperation = systemSettings.channel_operation || { state: 'idle' };
    const recentChannelTarget = Number(recentChannelOperation.target_channel);
    const recentChannelOperationText = recentChannelOperation.state === 'idle'
        ? '暂无信道操作'
        : Number.isInteger(recentChannelTarget) &&
            recentChannelTarget >= 11 && recentChannelTarget <= 26
            ? `切换至信道 ${recentChannelTarget}`
            : '自动选择信道';
    const recentChannelStateText = recentChannelOperation.state === 'pending'
        ? '等待执行'
        : recentChannelOperation.state === 'success'
            ? '执行成功'
            : recentChannelOperation.state === 'failed'
                ? '执行失败'
                : '暂无记录';
    return (
        <div className={`gateway-shell${activeTab === 'node-settings' ? ' node-settings-shell' : ''}`} style={styles.container}>

            {/* 左侧导航 */}
            <GatewaySidebar activeTab={activeTab} onSelect={tab => {
                setActiveTab(tab);
                if (tab === 'devices') loadMonitoringSettings();
            }} />


            {/* 右侧内容 */}
            <div className="gateway-content" style={styles.content}>

                {activeTab === 'logs' && <GatewayLogs nodes={whitelist} />}
                {activeTab === 'groups' && <NodeGroups nodes={whitelist} onSettings={openNodeSampling}
                    renderNode={(dev, index) => <CompactNodeCard dev={dev} index={index} parseMagData={parseMagData} />} />}
                {activeTab === 'home' && (
                    <div className="home-page">
                        <section className="home-overview" aria-label="概览">
                            <div className="home-node-stats">
                                {[
                                    { title: '节点总数', count: whitelist.length, hint: '已登记节点', icon: Network, tone: 'total' },
                                    { title: '在线节点数', count: onlineNodeCount, hint: 'Online', icon: Wifi, tone: 'online' },
                                    { title: '离网节点数', count: offlineNodeCount, hint: 'Offline', icon: WifiOff, tone: 'offline' }
                                ].map(({ title, count, hint, icon: Icon, tone }) => (
                                    <div key={tone} className={`home-node-stat home-node-stat--${tone}`}>
                                        <div>
                                            <span className="home-node-stat-label">{title}</span>
                                            <strong className="home-node-stat-count">{count}</strong>
                                            <span className="home-node-stat-hint">{hint}</span>
                                        </div>
                                        <Icon size={24} strokeWidth={1.8} aria-hidden="true" />
                                    </div>
                                ))}
                            </div>
                        </section>
                        <HomeNodes nodes={whitelist} activities={homeActivity.activities}
                            observed={homeActivity.observed} onOpen={() => setActiveTab('overview')} />
                    </div>
                )}

                {/* ==================================================
                 * 节点状态
                 * ================================================== */}
                {activeTab === 'overview' && (
                    <div>

                        {whitelist.length === 0 ? (
                            <div style={styles.emptyState}>
                                <div
                                    style={{
                                        fontSize: '40px',
                                        marginBottom: '15px'
                                    }}
                                >
                                    📭
                                </div>

                                <h3>暂无任何节点</h3>

                                <p>
                                    请前往左侧【节点管理】模块添加传感器节点
                                </p>
                            </div>
                        ) : (
                            <NodeDetailsList devices={whitelist} selectedTrend={selectedTrend}
                                onCloseTrend={() => setSelectedTrend(null)} renderNode={(dev, index) => (
                                    <NodeCard
                                        key={dev.mac || index}
                                        dev={dev}
                                        index={index}
                                        collectionStatus={collectionStatus[dev.mac] || ''}
                                        samplingRate={dev.vibration_sampling_rate || defaultMonitoringPolicy.vibration_sampling_rate}
                                        onRemove={() => handleRemoveDevice(dev.mac)}
                                        onTrend={metric => setSelectedTrend(previous => previous?.mac === dev.mac && previous.metric === metric ? null : { mac: dev.mac, metric })}
                                        activeTrend={selectedTrend?.mac === dev.mac ? selectedTrend.metric : null}
                                    />
                                )} />
                        )}
                    </div>
                )}


                {/* ==================================================
                 * 节点管理
                 * ================================================== */}
                {activeTab === 'devices' && (
                    <div>


                        {/* 操作区 */}
                        <div
                            className="ui-toolbar node-admission-toolbar"
                            style={{
                                marginBottom: '20px',
                                display: 'flex',
                                justifyContent: 'space-between',
                                alignItems: 'center',
                                gap: '20px',
                                flexWrap: 'wrap'
                            }}
                        >
                            <div>
                                {isScanning ? (
                                    <button
                                        type="button" className="ui-button ui-button-danger"
                                        onClick={handleStopScan} disabled={joinState.pending}
                                    >
                                        <CircleStop size={16} aria-hidden="true" />禁止节点加入
                                    </button>
                                ) : (
                                    <button
                                        type="button" className="ui-button ui-button-success"
                                        onClick={handleStartScan} disabled={joinState.pending}
                                    >
                                        <RadioTower size={16} aria-hidden="true" />允许节点加入
                                    </button>
                                )}
                            </div>

                            <div className="node-manual-add">
                                <input
                                    className="ui-input" aria-label="手动输入节点 MAC"
                                    placeholder="手动输入 MAC (如 00:12:4B...)"
                                    value={manualMac}
                                    onChange={(e) => setManualMac(e.target.value)}
                                    disabled={isScanning}
                                />

                                <button
                                    type="button" className="ui-button ui-button-accent"
                                    onClick={handleManualAdd}
                                    disabled={isScanning}
                                >
                                    <Plus size={16} aria-hidden="true" />手动添加
                                </button>
                            </div>
                        </div>


                        {joinState.error && <p role="alert">{joinState.error}</p>}


                        {showPendingAdmissions && <>
                            <hr className="node-management-divider" />
                            <PendingAdmissionPanel onAuthorized={refreshAuthorizedNodes} />
                        </>}
                        <hr className="node-management-divider" />
                        {/* 已登记节点 */}
                        <div>
                            <h3 style={{ marginTop: 0 }}>
                                节点列表
                            </h3>
                            <NodeColumnSelector columns={nodeColumns} onChange={changeNodeColumn} renaming={Boolean(renamingMac)} />
                            <div className="node-management-table-scroll">
                            <table className="node-management-table" style={styles.table}>
                                <thead>
                                    <tr>
                                        {NODE_COLUMNS.filter(([key]) => nodeColumns[key]).map(([key, label]) => <th key={key} style={styles.th} scope="col">{label}</th>)}
                                    </tr>
                                </thead>

                                <tbody>
                                    {whitelist.length === 0 ? (
                                        <tr>
                                            <td
                                                colSpan={NODE_COLUMNS.filter(([key]) => nodeColumns[key]).length}
                                                style={{
                                                    ...styles.td,
                                                    textAlign: 'center',
                                                    color: '#95a5a6',
                                                    padding: '30px'
                                                }}
                                            >
                                                暂无节点，请允许新节点加入，或手动登记设备
                                            </td>
                                        </tr>
                                    ) : (
                                        whitelist.map((dev, index) => (
                                            <tr
                                                key={dev.mac || index}
                                                style={{
                                                    backgroundColor:
                                                        dev.is_alarm
                                                            ? '#ffe6e6'
                                                            : 'transparent'
                                                }}
                                            >
                                                {nodeColumns.name && <td style={styles.td}>
                                                    {renamingMac === dev.mac ? (
                                                        <input
                                                            autoFocus
                                                            maxLength={32}
                                                            value={renameDraft}
                                                            disabled={savingNameMac === dev.mac}
                                                            onChange={(event) =>
                                                                setRenameDraft(event.target.value)
                                                            }
                                                            onKeyDown={(event) => {
                                                                if (event.key === 'Enter')
                                                                {
                                                                    saveDeviceName(dev);
                                                                }
                                                                else if (event.key === 'Escape')
                                                                {
                                                                    cancelRenameDevice();
                                                                }
                                                            }}
                                                            style={{
                                                                ...styles.input,
                                                                width: '220px',
                                                                maxWidth: '100%',
                                                                boxSizing: 'border-box'
                                                            }}
                                                            placeholder="输入节点名称"
                                                        />
                                                    ) : (
                                                        <strong>
                                                            {dev.name || '未知节点'}
                                                        </strong>
                                                    )}

                                                    <div
                                                        style={{
                                                            marginTop: '4px',
                                                            fontSize: '12px',
                                                            color: '#95a5a6'
                                                        }}
                                                    >
                                                        型号：{dev.model_name || '未知'}
                                                    </div>
                                                </td>}

                                                {nodeColumns.mac && <td style={styles.td}>
                                                    {dev.mac}
                                                </td>}
                                                {['networkAddress', 'lqi', 'rssi', 'lastSeen', 'availability', 'power', 'battery'].filter(key => nodeColumns[key]).map(key =>
                                                    <td key={key} style={styles.td}><NodeManagementField field={key} node={dev} observed={nodeLastUplink[dev.mac]} now={gatewayClockTick} /></td>
                                                )}

                                                {nodeColumns.interview && <td style={styles.td}>
                                                    <details><summary>{interviewLabel(dev.interview)}</summary>
                                                        <pre style={{ whiteSpace: 'pre-wrap', minWidth: 200 }}>{interviewSummary(dev.interview)}</pre>
                                                        <button type="button" onClick={() => retryInterview(dev.mac)}>重新识别</button>
                                                    </details>
                                                </td>}
                                                {nodeColumns.actions && <td style={styles.td}>
                                                    <NodeActions renaming={renamingMac === dev.mac} saving={savingNameMac === dev.mac}
                                                        customName={dev.custom_name} onSettings={() => openNodeSampling(dev)}
                                                        onRename={() => startRenameDevice(dev)} onDelete={() => handleRemoveDevice(dev.mac)}
                                                        onRestore={() => restoreDefaultDeviceName(dev)} onSave={() => saveDeviceName(dev)}
                                                        onCancel={cancelRenameDevice} />
                                                </td>}
                                            </tr>
                                        ))
                                    )}
                                </tbody>
                            </table>
                            </div>
                        </div>
                    </div>
                )}


                {/* ==================================================
                 * 网关状态（仅使用现有前端可获得的真实信息）
                 * ================================================== */}
                {activeTab === 'node-settings' && samplingNode && (
                    <NodeProfile node={whitelist.find(device => device.mac === samplingNode.mac) || samplingNode}
                        nodes={whitelist} onSelect={openNodeSampling} onRename={requestDeviceRename} onDescription={requestDeviceDescription}
                        observed={nodeLastUplink[samplingNode.mac]} now={gatewayClockTick} saving={samplingSaving} onBack={closeNodeSampling}>
                        {samplingLoading ? <p role="status">正在读取该节点已保存的配置…</p>
                            : samplingDraft ? <NodeSamplingCard policy={samplingDraft} notice={samplingNotice}
                                busy={samplingSaving} onFieldChange={updateNodeSampling} onSave={saveNodeSampling} />
                            : <p role="alert" className="compact-error">{samplingNotice}</p>}
                    </NodeProfile>
                )}

                {activeTab === 'gateway-status' && (
                    <div>

                        <div style={{
                            display: 'grid',
                            gridTemplateColumns: statusColumns,
                            gap: '16px',
                            marginTop: '20px'
                        }}>
                            <StatusMetric
                                title="网关运行情况"
                                value={gatewayStatus.apiConnected == null
                                    ? '检测中'
                                    : gatewayStatus.apiConnected && gatewayStatus.wsConnected
                                        ? '运行正常'
                                        : '运行异常'}
                                hint={`后端服务：${gatewayStatus.apiConnected == null
                                    ? '检测中'
                                    : gatewayStatus.apiConnected ? '正常' : '无法访问'} · 实时通道：${gatewayStatus.apiConnected == null
                                    ? '检测中'
                                    : gatewayStatus.wsConnected
                                        ? '已连接'
                                        : gatewayStatus.apiConnected ? '正在重连' : '已断开'}`}
                                tone={gatewayStatus.apiConnected == null
                                    ? 'warning'
                                    : gatewayStatus.apiConnected && gatewayStatus.wsConnected
                                        ? 'success'
                                        : 'danger'}
                            />
                            <StatusMetric
                                title="已登记节点"
                                value={`${onlineNodeCount} 在线 / ${whitelist.length} 台`}
                                hint="来自当前设备数据库和实时状态"
                                tone={onlineNodeCount > 0 ? 'success' : 'neutral'}
                            />
                            <StatusMetric
                                title="网关访问地址"
                                value={window.location.hostname}
                                hint={`前端端口：${window.location.port || '80'}`}
                            />
                            <StatusMetric
                                title="网关系统时间"
                                value={gatewayClockAvailable
                                    ? formatStatusTime(gatewayClockNowMs)
                                    : '等待网关时间'}
                                hint={!gatewayClockAvailable
                                    ? gatewayStatus.timeSyncError || '正在读取并校准网关时间'
                                    : gatewayClockStale
                                        ? `时间信息可能已过期 · 最后校准：${formatStatusTime(gatewayStatus.serverTimeCapturedAt)}`
                                        : gatewayStatus.timeAligned
                                            ? `已与电脑时间对齐 · 偏差：${gatewayStatus.timeOffsetMs >= 0 ? '+' : ''}${gatewayStatus.timeOffsetMs} ms · 每30秒校准`
                                            : `${gatewayStatus.timeSyncError || '与电脑时间存在偏差'} · 每30秒校准`}
                                tone={!gatewayClockAvailable
                                    ? 'warning'
                                    : gatewayClockStale
                                        ? 'danger'
                                        : gatewayStatus.timeAligned ? 'success' : 'warning'}
                            />
                            <StatusMetric
                                title="CPU使用率"
                                value={Number.isFinite(systemResources.data?.cpu_usage_percent)
                                    ? `${systemResources.data.cpu_usage_percent.toFixed(1)}%`
                                    : systemResources.data ? '采样中' : '--'}
                                hint={systemResources.error
                                    ? `读取失败：${systemResources.error}`
                                    : systemResources.data
                                        ? `当前后端Linux主机 · 最近更新：${formatStatusTime(systemResources.lastUpdated)}`
                                        : '等待后端实时数据'}
                            />
                            <StatusMetric
                                title="内存使用"
                                value={systemResources.data &&
                                    Number.isFinite(systemResources.data.memory_used_bytes) &&
                                    Number.isFinite(systemResources.data.memory_total_bytes)
                                    ? `${formatResourceBytes(systemResources.data.memory_used_bytes)} / ${formatResourceBytes(systemResources.data.memory_total_bytes)}`
                                    : '--'}
                                hint={Number.isFinite(systemResources.data?.memory_usage_percent)
                                    ? `使用率 ${systemResources.data.memory_usage_percent.toFixed(1)}%`
                                    : systemResources.error
                                        ? `读取失败：${systemResources.error}`
                                        : '等待后端实时数据'}
                            />
                            <StatusMetric
                                title="磁盘使用"
                                value={systemResources.data &&
                                    Number.isFinite(systemResources.data.disk_used_bytes) &&
                                    Number.isFinite(systemResources.data.disk_total_bytes)
                                    ? `${formatResourceBytes(systemResources.data.disk_used_bytes)} / ${formatResourceBytes(systemResources.data.disk_total_bytes)}`
                                    : '--'}
                                hint={Number.isFinite(systemResources.data?.disk_usage_percent)
                                    ? `使用率 ${systemResources.data.disk_usage_percent.toFixed(1)}%`
                                    : systemResources.error
                                        ? `读取失败：${systemResources.error}`
                                        : '等待后端实时数据'}
                            />
                            <StatusMetric
                                title="网关运行时间"
                                value={formatGatewayUptime(systemResources.data?.uptime_seconds)}
                                hint={systemResources.error
                                    ? `读取失败：${systemResources.error}`
                                    : '当前后端Linux主机启动时长'}
                            />
                            <StatusMetric
                                title="数据库大小"
                                value={formatResourceBytes(systemResources.data?.database_size_bytes)}
                                hint={systemResources.error
                                    ? `读取失败：${systemResources.error}`
                                    : 'gateway.db 实际文件大小'}
                            />
                            <StatusMetric
                                title="最近状态检查"
                                value={formatStatusTime(gatewayStatus.lastApiCheck)}
                                hint="通过系统状态接口检查"
                            />
                        </div>
                    </div>
                )}


                {/* ==================================================
                 * 协调器状态
                 * ================================================== */}
                {activeTab === 'coordinator-status' && (
                    <div>

                        <div style={{
                            display: 'grid',
                            gridTemplateColumns: statusColumns,
                            gap: '16px',
                            marginTop: '20px'
                        }}>
                            <StatusMetric
                                title="协调器通信"
                                value={coordinatorOnline
                                    ? '在线'
                                    : gatewayStatus.apiConnected === false
                                        ? '后端无法连接'
                                        : systemSettings.serial_available === false
                                            ? '串口未连接'
                                            : '协调器应答超时'}
                                hint={coordinatorOnline
                                    ? `最近应答：${formatStatusTime(systemSettings.coordinator_last_seen_ms)}`
                                    : gatewayStatus.apiConnected === false
                                        ? '请检查后端服务和协调器串口'
                                        : systemSettings.serial_available === false
                                            ? '请检查协调器串口配置'
                                            : '超过15秒未收到新的网络信息'}
                                tone={coordinatorOnline
                                    ? 'success'
                                    : gatewayStatus.apiConnected === false
                                        ? 'danger'
                                        : 'warning'}
                            />
                            <StatusMetric
                                title="Zigbee 网络状态"
                                value={coordinatorOnline
                                    ? zigbeeNetworkStateText(systemSettings.network_state)
                                    : '状态未知'}
                                hint={!coordinatorOnline
                                    ? '协调器离线，不继续使用缓存的网络状态'
                                    : zigbeeNetworkReady
                                        ? '协调器已完成 Zigbee 建网'
                                        : '协调器正在处理 Zigbee 网络'}
                                tone={zigbeeNetworkReady
                                    ? 'success'
                                    : !coordinatorOnline || systemSettings.network_state === 'unknown'
                                        ? 'warning'
                                        : systemSettings.network_state === 'error'
                                            ? 'danger'
                                            : 'neutral'}
                            />
                            <StatusMetric
                                title="PAN ID"
                                value={systemSettings.pan_id == null
                                    ? '--'
                                    : `0x${Number(systemSettings.pan_id).toString(16).toUpperCase().padStart(4, '0')}`}
                                hint="当前实际Zigbee网络"
                            />
                            <StatusMetric
                                title="当前信道"
                                value={systemSettings.current_channel ?? '--'}
                                hint="协调器当前实际工作信道"
                            />
                            <StatusMetric
                                title="网络启动来源"
                                value={networkSourceText}
                                hint="恢复旧网络或自动扫描建网"
                            />
                            <StatusMetric
                                title="最近信道操作"
                                value={(
                                    <div>
                                        <div>{recentChannelOperationText}</div>
                                        <span style={{
                                            display: 'inline-block',
                                            marginTop: '9px',
                                            padding: '4px 9px',
                                            borderRadius: '999px',
                                            fontSize: '12px',
                                            fontWeight: 700,
                                            color: recentChannelOperation.state === 'failed'
                                                ? '#c0392b'
                                                : recentChannelOperation.state === 'success'
                                                    ? '#188a55'
                                                    : recentChannelOperation.state === 'pending'
                                                        ? '#ad7610'
                                                        : '#71808f',
                                            background: 'rgba(255,255,255,0.72)'
                                        }}>
                                            {recentChannelStateText}
                                        </span>
                                    </div>
                                )}
                                hint={recentChannelOperation.message ||
                                    (recentChannelOperation.state === 'pending' &&
                                     Number(recentChannelOperation.total_nodes) > 0
                                        ? `已通知${Number(recentChannelOperation.prepared_nodes) || 0}/${Number(recentChannelOperation.total_nodes)}个节点`
                                        : '信道操作由设置页面提交')}
                                tone={recentChannelOperation.state === 'failed'
                                    ? 'danger'
                                    : recentChannelOperation.state === 'pending'
                                        ? 'warning'
                                        : recentChannelOperation.state === 'success'
                                            ? 'success'
                                            : 'neutral'}
                            />
                            <StatusMetric
                                title="网络节点"
                                value={`${onlineNodeCount} 在线 / ${whitelist.length} 台`}
                                hint="当前已登记节点统计"
                            />
                            <StatusMetric
                                title="最近协调器应答"
                                value={formatStatusTime(systemSettings.coordinator_last_seen_ms)}
                                hint="后端每5秒自动查询一次"
                            />
                        </div>
                    </div>
                )}


                {/* ==================================================
                 * 设置
                 * ================================================== */}
                {activeTab === 'settings' && (
                    <SettingsPage>


                        <div
                            style={{
                                display: 'grid',
                                gridTemplateColumns: overviewColumns,
                                gap: '20px',
                                marginTop: '20px',
                                alignItems: 'start'
                            }}
                        >
                            {/* Zigbee自动选信道或手动执行全网迁移。 */}
                            <div style={{ ...styles.card, gridColumn: '1 / -1' }}>
                                <h3 style={{ marginTop: 0 }}>
                                    📡 Zigbee 网络信息
                                </h3>

                                <div style={styles.infoGrid3}>
                                    <div style={styles.infoBox}>
                                        <div style={styles.infoTitle}>PAN ID</div>
                                        <div style={styles.infoValue}>
                                            {systemSettings.pan_id == null
                                                ? '--'
                                                : `0x${Number(systemSettings.pan_id)
                                                    .toString(16)
                                                    .toUpperCase()
                                                    .padStart(4, '0')}`}
                                        </div>
                                    </div>

                                    <div style={styles.infoBox}>
                                        <div style={styles.infoTitle}>当前实际信道</div>
                                        <div style={styles.infoValue}>
                                            {systemSettings.current_channel ?? '--'}
                                        </div>
                                    </div>

                                    <div style={styles.infoBox}>
                                        <div style={styles.infoTitle}>扫描配置</div>
                                        <select value={channelDraft}
                                            onChange={event => { channelDraftEdited.current = true; setChannelDraft(event.target.value); }}
                                            style={{ ...styles.input, width: '100%', margin: '7px 0 0', boxSizing: 'border-box' }}>
                                            <option value="auto">自动选择（默认）</option>
                                            <optgroup label="首选信道">
                                                {[11, 15, 20, 25].map(channel =>
                                                    <option key={channel} value={channel}>信道 {channel}（首选）</option>)}
                                            </optgroup>
                                            <optgroup label="次选信道">
                                                {[12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 24, 26].map(channel =>
                                                    <option key={channel} value={channel}>信道 {channel}（次选）</option>)}
                                            </optgroup>
                                        </select>
                                    </div>
                                </div>

                                <p
                                    style={{
                                        marginBottom: 0,
                                        marginTop: '18px',
                                        color: '#7f8c8d',
                                        fontSize: '13px',
                                        lineHeight: 1.7
                                    }}
                                >
                                    自动配置在下次建网时扫描11～26信道；手动选择时，空网络直接切换。
                                    已有节点时先等待各节点在周期唤醒窗口接收迁移通知，全部准备完成后
                                    协调器才执行最终切换。正常迁移不会改变节点短地址。
                                </p>
                                <div style={{ marginTop: '16px', textAlign: 'right' }}>
                                    {channelSaveNotice && (
                                        <span style={{
                                            marginRight: '14px',
                                            color: channelSaveNotice.startsWith('提交失败')
                                                ? '#c0392b'
                                                : '#60758a',
                                            fontSize: '13px'
                                        }}>
                                            {channelSaveNotice}
                                        </span>
                                    )}
                                    <button type="button"
                                        disabled={channelAction || settingsLoading ||
                                            systemSettings.channel_operation?.state === 'pending'}
                                        onClick={saveChannelStrategy}
                                        style={{
                                            ...styles.button,
                                            margin: 0,
                                            backgroundColor: '#2d7ff9',
                                            opacity: channelAction || settingsLoading ||
                                                systemSettings.channel_operation?.state === 'pending'
                                                ? 0.62 : 1
                                        }}>
                                        {channelAction
                                            ? '正在提交...'
                                            : systemSettings.channel_operation?.state === 'pending'
                                                ? '已有信道操作等待执行'
                                                : '保存并应用信道配置'}
                                    </button>
                                </div>
                            </div>
                        </div>
                    </SettingsPage>
                )}

            </div>
        </div>
    );
}
