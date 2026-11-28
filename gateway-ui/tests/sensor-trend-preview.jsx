import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import Card from '../src/NodeDetails.jsx';
import NodeDetailsList from '../src/NodeDetailsList.jsx';
import '../src/App.css';

// Isolated UI fixtures only: never written to the live database or node state.
const rows = Array.from({ length: 40 }, (_, index) => ({ batch_id: 100+index, timestamp: 1792396800+index*300,
    temperature: 26+Math.sin(index/4)*0.5, digital_db: 67+Math.sin(index/3)*3,
    analog_db: 70+Math.cos(index/3)*4, mag_x: 50+Math.sin(index/3)*10,
    mag_y: -240+Math.cos(index/4)*12, mag_z: -180+Math.sin(index/4)*8 }));
let mode='normal';
window.fetch = async (_url, options) => {
    if (options?.signal.aborted) throw new DOMException('Aborted', 'AbortError');
    if (mode==='error') throw new Error('Fixture unavailable');
    return { ok: true, json: async () => ({ status: 'success', data: mode==='empty' ? [] : mode==='single' ? rows.slice(0,1) : rows }) };
};
const dev = { mac: 'fixture', name: 'HY-Sensor-0E54', status: 'Online', lqi: 145, rssi: -50, power_source: 4,
    temp: '26.63', digital_db: '67.00', analog_db: '69.00', mag: true };
export function Preview() {
    const [selected, setSelected] = useState(null), [version, setVersion] = useState(0);
    return <main style={{ padding: 24 }}>
        <label style={{ fontSize: 12, color: '#8895a8' }}>独立测试数据　<select aria-label="测试状态" onChange={event => { mode=event.target.value; setVersion(value=>value+1); }}>
            <option value="normal">40 条记录</option><option value="single">单条记录</option><option value="empty">无数据</option><option value="error">读取失败</option>
        </select></label>
        <NodeDetailsList key={version} devices={[dev]} selectedTrend={selected ? { mac: dev.mac, metric: selected } : null}
            onCloseTrend={() => setSelected(null)} renderNode={() => <Card dev={dev} index={0}
                parseMagData={() => ({ x: '57.9', y: '-246.8', z: '-187.2' })}
                onTrend={metric => setSelected(previous => previous===metric ? null : metric)} activeTrend={selected} />} />
    </main>;
}
createRoot(document.getElementById('root')).render(<Preview />);
