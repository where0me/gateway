import { useCallback, useEffect, useRef, useState } from 'react';

const states = { uploading: '开始上传', complete: '上传完成', error: '采集或上传失败' };
export function activityText(event, previous = {}) {
    const change = (label, key, value, unit = '') => `${label}：${previous[key] != null && previous[key] !== '--' ? `${previous[key]} → ` : ''}${value}${unit}`;
    switch (event.type) {
        case 'status_update': return `网络状态：${event.status}`;
        case 'temp_update': return change('温度', 'temp', event.temp, ' ℃');
        case 'mag_update': return change('磁场', 'mag', event.mag, ' μT');
        case 'sound_update': return change(event.source === 'digital' ? '数字麦克风声级' : '超声麦克风声级', event.source === 'digital' ? 'digital_db' : 'analog_db', event.db, ' dB SPL');
        case 'monitor_update': {
            if (event.collection_state === 'idle') {
                const valid = value => Number.isInteger(value) && value >= 0 && value <= 255;
                if (!valid(event.lqi) || event.lqi === previous.lqi) return null;
                return valid(previous.lqi) ? change('LQI', 'lqi', event.lqi) : `LQI：${event.lqi}`;
            }
            const state = states[event.collection_state];
            return state ? `${state}${event.batch_id ? ` · 批次 ${event.batch_id}` : ''}` : null;
        }
        // Raw commit precedes the separate feature-summary ACK in the current
        // node. Report completion only on its final monitor-state confirmation.
        case 'collection_data_ready':
        case 'vibration_upload_progress': return null;
        case 'name_update': return `节点名称已更新：${event.name || event.model_name || event.mac}`;
        default: return null;
    }
}

export function useHomeActivity(nodes) {
    const [activities, setActivities] = useState([]);
    const [observed, setObserved] = useState({});
    const latest = useRef(new Map());
    const sequence = useRef(0);
    useEffect(() => { latest.current = new Map(nodes.map(node => [node.mac, node])); }, [nodes]);
    const record = useCallback(event => {
        const node = latest.current.get(event.mac);
        if (!node) return;
        const now = Date.now();
        const text = activityText(event, node);
        // Suppressed collection/idle/progress events still prove node activity.
        if (!text && !['vibration_upload_progress', 'collection_data_ready', 'monitor_update'].includes(event.type)) return;
        setObserved(previous => ({ ...previous, [event.mac]: now }));
        if (!text) return;
        const item = { id: ++sequence.current, mac: event.mac, name: node.name || node.mac, text, time: now };
        setActivities(previous => [item, ...previous].slice(0, 20));
    }, []);
    return { activities, observed, record };
}
