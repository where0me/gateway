import React from 'react';
import { Battery, HelpCircle, Plug, Signal, WifiOff } from 'lucide-react';
import { NODE_COLUMNS, batteryText, nodeLastSeen, relativeSeen, networkAddress } from './nodeManagementColumns.js';

export function NodeColumnSelector({ columns, onChange, renaming }) {
    const count = NODE_COLUMNS.filter(([key]) => columns[key]).length;
    return <fieldset className="node-column-selector"><legend>列：</legend>{NODE_COLUMNS.map(([key, label]) =>
        <label key={key}><input type="checkbox" checked={columns[key]}
            disabled={(count === 1 && columns[key]) || (renaming && ['name', 'actions'].includes(key))}
            onChange={event => onChange(key, event.target.checked)} />{label}</label>
    )}</fieldset>;
}

export function NodeManagementField({ field, node, observed, now }) {
    const online = String(node.status).toLowerCase() === 'online';
    if (field === 'networkAddress') {
        const address = networkAddress(node.short_addr);
        return <span title={address ? `十进制：${address.decimal}${online ? '' : '（离线前记录）'}` : undefined}>{address?.hex || '未上报'}</span>;
    }
    if (field === 'lqi') {
        const valid = Number.isInteger(node.lqi) && node.lqi >= 0 && node.lqi <= 255;
        return <span className="node-table-signal" title={!online ? '节点离线，当前 LQI 不可用' : '链路质量：0～255'}>
            {online ? <Signal size={16} className={!valid ? 'signal-offline' : node.lqi < 50 ? 'signal-low' : 'signal-good'} /> : <WifiOff size={16} className="signal-offline" />}
            {online ? valid ? node.lqi : '未上报' : '离线'}</span>;
    }
    if (field === 'rssi') {
        const valid = Number.isInteger(node.rssi) && node.rssi > -128 && node.rssi <= 127;
        return <span title={online ? '协调器接收节点上行的信号强度，单位 dBm' : '节点离线，当前 RSSI 不可用'}>
            {online ? valid ? `${node.rssi} dBm` : '未上报' : '离线'}</span>;
    }
    if (field === 'lastSeen') {
        const time = nodeLastSeen(node, observed);
        return <time dateTime={time ? new Date(time).toISOString() : undefined}
            title={time ? `最近收到节点上报：${new Date(time).toLocaleString('zh-CN', { hour12: false })}` : '尚无节点上报时间'}>{relativeSeen(time, now)}</time>;
    }
    if (field === 'availability') return <span className={`node-table-availability${online ? ' is-online' : ''}`}>● {online ? '在线' : '离线'}</span>;
    if (field === 'power') {
        const battery = node.power_source === 3;
        const external = [1, 2, 4, 5, 6].includes(node.power_source);
        const Icon = battery ? Battery : external ? Plug : HelpCircle;
        return <span className="node-table-signal" title={node.power_source === 4 ? '外部直流供电（当前硬件使用 ST-Link）' : undefined}><Icon size={16} />{battery ? '电池' : external ? '电源' : '未上报'}</span>;
    }
    if (field === 'battery') return <span title="仅在节点上报电池百分比时显示电量">{batteryText(node)}</span>;
    return null;
}
