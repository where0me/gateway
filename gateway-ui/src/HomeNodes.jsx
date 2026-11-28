import React, { useEffect, useState } from 'react';
import { Battery, CircuitBoard, HelpCircle, Plug, Signal, WifiOff } from 'lucide-react';

function ago(time, now) {
    if (!time) return '暂无活动记录';
    const seconds = Math.max(0, Math.floor((now - time) / 1000));
    if (seconds < 60) return seconds < 5 ? '刚刚' : `${seconds} 秒前`;
    if (seconds < 3600) return `${Math.floor(seconds / 60)} 分钟前`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)} 小时前`;
    return `${Math.floor(seconds / 86400)} 天前`;
}

export default function HomeNodes({ nodes, activities, observed, onOpen }) {
    const authorized = new Set(nodes.map(node => node.mac));
    const visibleActivities = activities.filter(item => authorized.has(item.mac));
    const [now, setNow] = useState(Date.now);
    useEffect(() => {
        const timer = setInterval(() => setNow(Date.now()), 10000);
        return () => clearInterval(timer);
    }, []);
    return <>
        <section className="home-activity" aria-labelledby="home-activity-title">
            <div className="home-section-heading"><h3 id="home-activity-title">最新活动</h3><small>本次页面会话 · 最近20条</small></div>
            {visibleActivities.length ? <ul className="home-activity-list">{visibleActivities.map(item => <li key={item.id}>
                <span><strong>{nodes.find(node => node.mac === item.mac)?.name || item.name}</strong><span className="home-activity-message"> — {item.text}</span></span>
                <time dateTime={new Date(item.time).toISOString()}>{new Date(item.time).toLocaleString('zh-CN', { hour12: false })}</time>
            </li>)}</ul> : <p className="home-muted">暂无最新活动，等待节点实时上报。</p>}
        </section>
        <section className="home-devices" aria-labelledby="home-devices-title">
            <div className="home-section-heading"><h3 id="home-devices-title">网络节点</h3><small>{nodes.length} 台已登记节点</small></div>
            {!nodes.length ? <p className="home-muted">暂无节点，请在「节点管理」中添加设备。</p> : <div className="home-device-grid">{nodes.map(node => {
                const online = String(node.status).toLowerCase() === 'online';
                const lqi = Number.isInteger(node.lqi) && node.lqi >= 0 && node.lqi <= 255 ? node.lqi : null;
                const power = { 1: '市电供电', 2: '三相供电', 3: '电池供电', 4: '外部直流供电（ST-Link）', 5: '应急供电', 6: '应急供电' }[node.power_source];
                const PowerIcon = node.power_source === 3 ? Battery : power ? Plug : HelpCircle;
                const time = observed[node.mac] || Number(node.last_collection) * 1000;
                return <button type="button" key={node.mac} className={`home-device-card${online ? '' : ' is-offline'}${node.is_alarm ? ' is-alarm' : ''}`} onClick={() => onOpen(node.mac)} aria-label={`查看 ${node.name || node.mac} 的节点状态`}>
                    <CircuitBoard className="home-device-icon" size={32} strokeWidth={1.5} aria-hidden="true" />
                    <div className="home-device-info"><strong title={node.name || node.mac}>{node.name || node.mac}</strong><span>{node.model_name || '未知型号'}</span><small title={time ? `${observed[node.mac] ? '最近活动' : '最近采集'}：${new Date(time).toLocaleString()}` : '未收到活动或采集时间'}>{observed[node.mac] ? '活动 ' : time ? '采集 ' : ''}{ago(time, now)}</small></div>
                    <footer><span className={`home-device-status${online ? ' is-online' : ''}`}>● {online ? 'Online' : 'Offline'}</span>
                        <span className="home-device-badge" title={online ? `LQI：${lqi ?? '未上报'}` : '节点离线，当前 LQI 不可用'} aria-label={online ? `LQI ${lqi ?? '未上报'}` : 'LQI：节点离线'}>
                            {online ? <Signal size={14} className={lqi === null ? '' : lqi < 50 ? 'signal-low' : 'signal-good'} aria-hidden="true" /> : <WifiOff size={14} className="signal-offline" aria-hidden="true" />}{online ? lqi ?? 'N/A' : '离线'}
                        </span>
                        <span className="home-device-badge" title={power || '电源类型未上报'} aria-label={power || '电源类型未上报'}><PowerIcon size={15} aria-hidden="true" /></span>
                    </footer>
                </button>;
            })}</div>}
        </section>
    </>;
}
