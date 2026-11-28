import React, { useEffect, useState } from 'react';
import { Battery, ChartLine, Plug, Signal, Trash2, WifiOff } from 'lucide-react';
import { batteryText } from './nodeManagementColumns';
import { nodeCardView, uploadStatusText } from './nodeDetailsState.js';

async function readWaveform(dev) {
    const query = new URLSearchParams({ mac: dev.mac, raw: '1' });
    if (dev.collection_id) query.set('batch', String(dev.collection_id));
    const response = await fetch(`http://${window.location.hostname}:8080/api/device/vibration/latest?${query}`);
    const data = await response.json();
    if (!response.ok || data.status !== 'success' || !data.raw_available)
        throw new Error(data.msg || '原始数据尚未上传完成');
    if (dev.collection_id && Number(data.batch_id) !== Number(dev.collection_id))
        throw new Error('采集批次已更新，请刷新节点后重试');
    if (Number(data.display_step) !== 1 || data.samples?.length !== Number(data.sample_count))
        throw new Error('原始数据不完整，请稍后重试');
    return { ...dev, vibration_sampling_rate: data.sample_rate_hz,
        vibration_sample_count: data.sample_count,
        vibration_samples: data.samples.map((sample, index) => ({ index,
            time: index / data.sample_rate_hz, x_raw: sample[0], y_raw: sample[1], z_raw: sample[2],
            x: sample[0] * 0.000488, y: sample[1] * 0.000488, z: sample[2] * 0.000488 })) };
}

function downloadRaw(dev) {
    const rows = ['sample_index,time_s,x_raw,y_raw,z_raw', ...dev.vibration_samples.map(sample =>
        [sample.index, sample.time.toFixed(8), sample.x_raw, sample.y_raw, sample.z_raw].join(','))];
    const url = URL.createObjectURL(new Blob(['\uFEFF' + rows.join('\n')], { type: 'text/csv;charset=utf-8' }));
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = `${String(dev.name).replace(/[^\w\u4e00-\u9fa5-]/g, '_')}-vibration.csv`;
    anchor.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
}

export default function CompactNodeCard({ dev, index, parseMagData, onRemove, onTrend, activeTrend }) {
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState('');
    const statusText = uploadStatusText(dev);
    dev = nodeCardView(dev);
    const online = String(dev.status).toLowerCase() === 'online';
    const mag = parseMagData(dev.mag);
    const complete = dev.waveform_available;
    const lqi = typeof dev.lqi === 'number' && dev.lqi >= 0 && dev.lqi <= 255 ? dev.lqi : null;
    const rssi = Number.isInteger(dev.rssi) && dev.rssi > -128 && dev.rssi <= 127 ? dev.rssi : null;
    const battery = batteryText(dev);
    const power = { 1: '市电供电', 2: '三相供电', 3: '电池供电', 4: '外部供电', 5: '应急供电', 6: '应急供电' }[dev.power_source] || '未上报';
    const linkTime = dev.link_updated_at ? new Date(dev.link_updated_at * 1000).toLocaleString() : '未知';
    const value = (item, unit) => item !== undefined && item !== null && item !== '--' && item !== '' ? `${item} ${unit}` : '--';
    const trendButton = (metric, label) => onTrend && <button type="button"
        className={`compact-trend-button${activeTrend === metric ? ' is-active' : ''}`}
        title={`查看${label}趋势`} aria-label={`查看${label}趋势`} aria-pressed={activeTrend === metric}
        onClick={() => onTrend(metric)}><ChartLine size={14} aria-hidden="true" /></button>;
    async function action(view) {
        setError('');
        if (view) {
            const url = new URL(window.location.href);
            url.search = new URLSearchParams({ waveform: dev.mac,
                name: dev.name || '节点', batch: dev.collection_id || '' }).toString();
            url.hash = '';
            const target = window.open(url.href, '_blank', 'popup,width=1100,height=650');
            if (!target) { setError('浏览器阻止了新窗口，请允许弹出窗口后重试'); return; }
            target.opener = null;
            return;
        }
        setBusy(true);
        try {
            const data = await readWaveform(dev);
            downloadRaw(data);
        } catch (problem) { setError(problem.message); }
        finally { setBusy(false); }
    }
    return <article className={`compact-node${dev.is_alarm ? ' compact-node-alarm' : ''}`}>
        <header className="compact-node-header">
            <div className="compact-node-icon" aria-hidden="true">◈</div>
            <div className="compact-node-identity"><h3 title={dev.name}>{dev.name || `节点 ${index + 1}`}</h3><small>{dev.model_name || 'HY-Sensor'}</small></div>
            <span className={`compact-node-status${online ? ' is-online' : ''}`}>● {online ? 'Online' : 'Offline'}</span>
        </header>
        <div className="compact-sensors">
            <div><span>温度</span><strong className={dev.is_alarm ? 'alarm-value' : ''}>{value(dev.temp, '℃')}</strong>{trendButton('temperature', '温度')}</div>
            <div><span>数字麦克风</span><strong>{value(dev.digital_db, 'dB SPL')}</strong>{trendButton('digital', '数字麦克风声级')}</div>
            <div><span>超声麦克风 <small>估算</small></span><strong>{value(dev.analog_db, 'dB SPL')}</strong>{trendButton('ultrasonic', '超声麦克风声级')}</div>
            <div className="compact-magnetic"><span>磁场 <small>μT</small></span><strong>{mag && !mag.raw ? `X ${mag.x} · Y ${mag.y} · Z ${mag.z}` : mag?.raw || '--'}</strong>{trendButton('magnetic', '磁场')}</div>
        </div>
        <section className="compact-vibration">
            <h4 title={dev.vibration_features_timestamp ? `振动特征：${new Date(dev.vibration_features_timestamp * 1000).toLocaleString()}` : undefined}>{dev.card_batch_active || dev.card_pending ? '本次振动特征' : '最近振动特征'} <small>加速度 g · 主频 Hz</small></h4>
            <div className="compact-table-scroll"><table><thead><tr>{['轴', 'RMS', '峰值', '峰峰值', '峭度', '主频'].map(label => <th key={label}>{label}</th>)}</tr></thead>
                <tbody>{['X', 'Y', 'Z'].map((axis, row) => <tr key={axis}><th>{axis}</th>{Array.from({ length: 5 }, (_, column) => {
                    const item = dev.vibration_features?.[row]?.[column];
                    return <td key={column}>{item != null && Number.isFinite(Number(item)) ? Number(item).toFixed(column === 4 ? 1 : 3) : '--'}</td>;
                })}</tr>)}</tbody></table></div>
        </section>
        <footer className="compact-node-footer">
            <div className="compact-upload" aria-live="polite">
                <span className={complete && !statusText.includes('等待') ? 'upload-complete' : ''}>{statusText}</span>
                {dev.card_uploading && dev.raw_upload_status !== 'complete' && dev.raw_total_packets > 0 && <progress aria-label="原始波形上传进度"
                    max={dev.raw_total_packets} value={Math.max(0, dev.raw_received_packets || 0)} />}
                <small title="采集开始通知到达网关的时间；排队和补传不改变此时间">{dev.capture_time
                    ? `采集时间：${dev.capture_time}` : dev.card_pending ? '本批采集时间未记录' : '采集时间未记录'}</small>
            </div>
            <div className="compact-wave-actions">
                <button disabled={!complete || busy} onClick={() => action(false)}>下载原始数据</button>
                <button className="primary" disabled={!complete || busy} onClick={() => action(true)}>在线波形图 ↗</button>
            </div>
            {error && <p role="alert" className="compact-error">{error}</p>}
            <div className="compact-device-tools">
                <div className="compact-device-metrics">
                <span className="compact-device-metric" title={!online ? '节点离线，当前 LQI 不可用' : lqi === null ? 'LQI：尚未上报' : `LQI：${lqi} / 255 · 最近测量：${linkTime}`} aria-label={online ? `LQI ${lqi ?? '未上报'}` : 'LQI：节点离线'}>
                    {online ? <Signal size={14} className={lqi === null ? 'metric-unknown' : lqi < 50 ? 'metric-low' : 'metric-good'} aria-hidden="true" /> : <WifiOff size={14} className="metric-unknown" aria-hidden="true" />}<span>LQI</span><strong>{online ? lqi ?? '--' : '--'}</strong>
                </span>
                <span className="compact-device-metric" title={`协调器接收节点上报时测得的 RSSI · 最近测量：${linkTime}`} aria-label={`RSSI ${online && rssi !== null ? `${rssi} dBm` : '未上报或离线'}`}>
                    <span>RSSI</span><strong>{online && rssi !== null ? `${rssi} dBm` : '--'}</strong>
                </span>
                <span className="compact-device-metric" title={dev.power_source === 4 ? '外部直流供电（当前硬件使用 ST-Link；不是实时电源电压测量）' : power} aria-label={`电源：${power}`}>
                    <Plug size={14} className={dev.power_source ? 'metric-good' : 'metric-unknown'} aria-hidden="true" />{power}
                </span>
                <span className="compact-device-metric" title={battery === 'N/A' ? '非电池供电，剩余电量不适用' : '仅显示节点真实上报的电量，不根据 RSSI 或供电类型估算'} aria-label={`剩余电量：${battery === 'N/A' ? '不适用' : battery}`}>
                    <Battery size={14} className="metric-unknown" aria-hidden="true" /><span>电量</span><strong>{battery === 'N/A' ? '不适用' : battery}</strong>
                </span>
                </div>
                <button type="button" className="compact-device-delete" onClick={onRemove} disabled={!onRemove} aria-label={`删除节点 ${dev.name || dev.mac}`} title="删除节点">
                    <Trash2 size={16} aria-hidden="true" />
                </button>
            </div>
        </footer>
    </article>;
}

export function WaveformPage({ Waveform }) {
    const [data, setData] = useState(null);
    const [error, setError] = useState('');
    const params = new URLSearchParams(window.location.search);
    const mac = params.get('waveform');
    const name = params.get('name') || '节点';
    const batch = params.get('batch');
    useEffect(() => {
        let cancelled = false;
        document.title = `${name} · 三轴振动波形`;
        readWaveform({ mac, name, collection_id: batch })
            .then(result => { if (!cancelled) setData(result); })
            .catch(problem => { if (!cancelled) setError(problem.message); });
        return () => { cancelled = true; };
    }, [mac, name, batch]);
    return <main className="waveform-page">
        <h2>{name} · 三轴振动波形</h2>
        <p>完整原始数据 · 三轴加速度</p>
        {error ? <p role="alert" className="compact-error">{error}</p>
            : data ? <Waveform dev={data} samplingRate={data.vibration_sampling_rate} /> : <p>正在加载完整波形…</p>}
    </main>;
}
