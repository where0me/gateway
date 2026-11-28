import React, { useEffect, useState } from 'react';
import { ChartLine, RefreshCw, X } from 'lucide-react';
import { normalizeTrendRecords, TREND_LIMIT, TREND_METRICS, trendBounds } from './sensorTrend.js';
import './SensorTrendPanel.css';

const captureTime = record => Number(record.captured_at) > 0 ? new Date(record.captured_at * 1000).toLocaleString() : '采集时间未记录';

export function SensorTrendChart({ records, metric }) {
    const [hovered, setHovered] = useState(null);
    const config = TREND_METRICS[metric];
    if (!config || !records.length) return null;
    const [low, high] = trendBounds(records, config.series);
    const width=480, height=220, left=54, right=18, top=18, bottom=32;
    const plotWidth=width-left-right, plotHeight=height-top-bottom;
    const pointX = index => left + (records.length === 1 ? plotWidth/2 : index*plotWidth/(records.length-1));
    const pointY = value => top + (high-Number(value))/(high-low)*plotHeight;
    const selected = Math.min(hovered ?? records.length-1, records.length-1);
    const record = records[selected];
    const ticks = [...new Set(Array.from({ length: Math.min(records.length, 4) }, (_, index) =>
        Math.round(index*(records.length-1)/Math.max(1, Math.min(records.length, 4)-1))))];
    return <>
        <div className="sensor-trend-legend">{config.series.map(series =>
            <span key={series.key}><i style={{ background: series.color }} />{series.label}</span>)}
            <small>{config.unit}</small>
        </div>
        <svg className="sensor-trend-chart" viewBox={`0 0 ${width} ${height}`} role="img"
            aria-label={`${config.label}变化趋势，共 ${records.length} 次采集`}>
            <title>{`${config.label}变化趋势`}</title>
            <desc>横轴为采集顺序，从旧到新；纵轴单位为{config.unit}。聚焦数据点可查看数值。</desc>
            {Array.from({ length: 5 }, (_, index) => {
                const value=high-index*(high-low)/4, y=pointY(value);
                return <g key={index}><line x1={left} y1={y} x2={width-right} y2={y} className="trend-grid-line" />
                    <text x={left-12} y={y+4} textAnchor="end">{value.toFixed(high-low < 1 ? 2 : 1)}</text></g>;
            })}
            {ticks.map(index => <text key={index} x={pointX(index)} y={height-16} textAnchor="middle">第 {index+1} 次</text>)}
            <line className="trend-cursor" x1={pointX(selected)} x2={pointX(selected)} y1={top} y2={height-bottom} />
            {records.map((item, index) => <rect key={item.batch_id} x={pointX(index)-Math.max(5, plotWidth/(records.length*2))}
                y={top} width={Math.max(10, plotWidth/records.length)} height={plotHeight} fill="transparent" pointerEvents="all"
                onMouseEnter={() => setHovered(index)} aria-hidden="true" />)}
            {config.series.map(series => <g key={series.key}>
                <polyline fill="none" pointerEvents="none" stroke={series.color} strokeWidth="2.5" strokeLinejoin="round" strokeLinecap="round"
                    points={records.map((item, index) => `${pointX(index)},${pointY(item[series.key])}`).join(' ')} />
                {records.map((item, index) => <circle key={item.batch_id} cx={pointX(index)} cy={pointY(item[series.key])}
                    r={index === selected ? 5 : 3.5} fill={series.color} stroke="white" strokeWidth="2"
                    tabIndex="0" role="img" aria-label={`第 ${index+1} 次，${series.label} ${Number(item[series.key]).toFixed(config.digits)} ${config.unit}`}
                    onMouseEnter={() => setHovered(index)} onFocus={() => setHovered(index)}>
                    <title>{`${captureTime(item)} · ${series.label}：${Number(item[series.key]).toFixed(config.digits)} ${config.unit}`}</title>
                </circle>)}
            </g>)}
        </svg>
        <div className="sensor-trend-reading">
            <div><strong>第 {selected+1} 次采集</strong><time title="采集开始通知到达网关的时间" dateTime={Number(record.captured_at) > 0 ? new Date(record.captured_at*1000).toISOString() : undefined}>
                {captureTime(record)}</time></div>
            <div className="sensor-trend-values">{config.series.map(series => <span key={series.key}>
                <i style={{ background: series.color }} />{series.label}<strong>{Number(record[series.key]).toFixed(config.digits)}</strong><small>{config.unit}</small>
            </span>)}</div>
            <small className="sensor-trend-batch">批次 {record.batch_id}</small>
        </div>
    </>;
}

function TrendPanelContent({ mac, name, metric, onClose }) {
    const [state, setState] = useState({ records: [], loading: true, error: '' });
    const config = TREND_METRICS[metric];
    useEffect(() => {
        if (!config || !mac) return;
        let active=true, timer, controller;
        async function refresh() {
            controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 10000);
            try {
                const query = new URLSearchParams({ mac, metric });
                const response = await fetch(`http://${window.location.hostname}:8080/api/device/collection_history?${query}`, { signal: controller.signal });
                const data = await response.json();
                if (!response.ok || data.status !== 'success') throw new Error(data.msg || '读取历史数据失败');
                if (active) setState({ records: normalizeTrendRecords(data.data, metric), loading: false, error: '' });
            } catch (problem) {
                if (active) setState(previous => ({ ...previous, loading: false,
                    error: problem.name === 'AbortError' ? '请求超时，正在重试' : '历史数据暂时无法读取，正在重试' }));
            } finally {
                clearTimeout(timeout);
                if (active) timer=setTimeout(refresh, 3000);
            }
        }
        refresh();
        return () => { active=false; clearTimeout(timer); controller?.abort(); };
    }, [mac, metric, config]);
    if (!config) return null;
    return <aside className="sensor-trend-panel" aria-label={`${name} ${config.label}趋势`}>
        <header className="sensor-trend-header">
            <span className="sensor-trend-heading-icon"><ChartLine size={20} aria-hidden="true" /></span>
            <div><h3>{config.label}<span>变化趋势</span></h3><p title={name}>{name}</p></div>
            <button type="button" className="sensor-trend-close" onClick={onClose} aria-label="关闭趋势图" title="关闭趋势图"><X size={18} /></button>
        </header>
        <div className="sensor-trend-meta"><span>最近 {state.records.length} / {TREND_LIMIT} 次采集</span>
            <small><RefreshCw size={12} aria-hidden="true" />每 3 秒更新</small></div>
        {state.error && <p className="sensor-trend-error" role="alert">{state.error}</p>}
        {state.records.length ? <SensorTrendChart records={state.records} metric={metric} />
            : <div className="sensor-trend-empty" role="status"><ChartLine size={34} aria-hidden="true" />
                <strong>{state.loading ? '正在读取采集历史…' : state.error ? '暂时无法显示趋势' : '暂无采集数据'}</strong>
                <span>{!state.loading && !state.error ? '节点上报后，这里会显示变化趋势' : '请稍候'}</span></div>}
    </aside>;
}

export default function SensorTrendPanel(props) {
    return <TrendPanelContent key={`${props.mac}:${props.metric}`} {...props} />;
}
