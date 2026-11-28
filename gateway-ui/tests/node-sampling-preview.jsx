import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import NodeProfile from '../src/NodeProfile.jsx';
import { NodeSamplingCard } from '../src/App.jsx';

// Isolated UI fixtures: no backend requests, database writes or radio commands.
const nodes = [
    { mac: '01:02:03:04:05:06:07:08', name: 'HY-Sensor-预览', model_name: 'HY-Sensor', status: 'Online', short_addr: 0x1234 },
    { mac: '11:12:13:14:15:16:17:18', name: '另一个预览节点', status: 'Offline' }
];
const initial = {
    id: 'preview', automatic_interval_minutes: 5, raw_upload_every_cycles: 1,
    vibration_enabled: true, vibration_window_seconds: 0.5,
    digital_microphone_enabled: true, digital_microphone_window_seconds: 1,
    analog_microphone_enabled: true, analog_microphone_window_seconds: 1
};
function Preview() {
    const [policy, setPolicy] = useState(initial);
    const [node, setNode] = useState(nodes[0]);
    const [narrow, setNarrow] = useState(false);
    const [busy, setBusy] = useState(false);
    const [notice, setNotice] = useState('');
    return <main style={{ padding: 24 }}>
        <div style={{ display: 'flex', gap: 12, marginBottom: 20, color: '#7a899d', fontSize: 12 }}>
            <span>独立布局预览 · 不会连接真实网关</span>
            <button type="button" onClick={() => setNarrow(value => !value)}>{narrow ? '宽屏预览' : '窄屏预览'}</button>
            <button type="button" onClick={() => setNotice('保存失败：预览错误状态')}>错误状态</button>
        </div>
        <div style={{ maxWidth: narrow ? 420 : 1800, margin: 'auto' }}>
            <NodeProfile node={node} nodes={nodes} now={Date.now()} saving={busy}
                onSelect={value => { setNode(value); setPolicy(initial); setNotice(''); }}
                onBack={() => setNotice('返回按钮正常（仅预览）')}>
                <NodeSamplingCard policy={policy} busy={busy} notice={notice}
                    onFieldChange={(id, field, value) => { setPolicy(previous => ({ ...previous, [field]: value })); setNotice(''); }}
                    onSave={() => {
                        setBusy(true);
                        setTimeout(() => { setBusy(false); setNotice('测试配置已保存（仅预览，不会下发）'); }, 1200);
                    }} />
            </NodeProfile>
        </div>
    </main>;
}
createRoot(document.getElementById('root')).render(<Preview />);
