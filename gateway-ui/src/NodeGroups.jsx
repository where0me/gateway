import React, { useCallback, useEffect, useState } from 'react';
import './NodeGroups.css';
import { GROUP_COLUMNS, GROUP_COLUMNS_KEY, readGroupColumns } from './groupColumns.js';
import { Settings, SquarePen, Trash2, Plus } from 'lucide-react';

export function GroupActions({ disabled, onSampling, onEdit, onDelete }) {
    return <div className="node-icon-actions group-icon-actions" role="group" aria-label="分组操作">
        {[
            ['采样参数配置', Settings, 'action-settings', onSampling],
            ['编辑分组', SquarePen, 'action-group-edit', onEdit],
            ['删除分组', Trash2, 'action-delete', onDelete]
        ].map(([label, Icon, tone, onClick]) => <button key={label} type="button" className={`node-icon-action ${tone}`}
            title={label} aria-label={label} disabled={disabled} onClick={onClick}><Icon size={16} strokeWidth={1.8} aria-hidden="true" /></button>)}
    </div>;
}

export function GroupColumnSelector({ columns, onChange }) {
    const count = GROUP_COLUMNS.filter(([key]) => columns[key]).length;
    return <fieldset className="node-column-selector"><legend>列：</legend>
        {GROUP_COLUMNS.map(([key, label]) => <label key={key}><input type="checkbox" checked={columns[key]}
            disabled={count === 1 && columns[key]} onChange={event => onChange(key, event.target.checked)} />{label}</label>)}
    </fieldset>;
}

const api = async (path, body, signal) => {
    const response = await fetch(`http://${window.location.hostname}:8080/api/groups${path}`, {
        signal, ...(body ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) } : {})
    });
    const data = await response.json();
    if (!response.ok || data.status !== 'success') throw new Error(data.msg || '分组操作失败');
    return data;
};

export default function NodeGroups({ nodes, renderNode, onSettings }) {
    const [groups, setGroups] = useState([]);
    const [loading, setLoading] = useState(true);
    const [error, setError] = useState('');
    const [selected, setSelected] = useState(null);
    const [draft, setDraft] = useState(null);
    const [search, setSearch] = useState('');
    const [busy, setBusy] = useState(false);
    const [sampling, setSampling] = useState(null);
    const [notice, setNotice] = useState('');
    const [columns, setColumns] = useState(() => readGroupColumns());
    function changeColumn(key, checked) {
        const next = { ...columns, [key]: checked };
        if (!Object.values(next).some(Boolean)) return;
        setColumns(next);
        try { localStorage.setItem(GROUP_COLUMNS_KEY, JSON.stringify(next)); } catch { /* Keep session preference. */ }
    }
    function cancelSampling() {
        if (busy) return;
        setSampling(null);
        setError('');
    }
    const load = useCallback(async (signal) => {
        const data = await api('', null, signal);
        setGroups(data.groups);
        setLoading(false);
    }, []);
    useEffect(() => {
        const controller = new AbortController();
        const refresh = () => load(controller.signal).catch(problem => {
            if (problem.name !== 'AbortError') { setError(problem.message); setLoading(false); }
        });
        refresh();
        const timer = setInterval(refresh, 5000);
        return () => { controller.abort(); clearInterval(timer); };
    }, [load]);
    useEffect(() => {
        if (!draft && !sampling) return undefined;
        const listener = event => { if (event.key === 'Escape' && !busy) { setDraft(null); setSampling(null); setError(''); } };
        window.addEventListener('keydown', listener);
        return () => window.removeEventListener('keydown', listener);
    }, [draft, sampling, busy]);

    const owners = new Map();
    groups.forEach(group => group.node_macs.forEach(mac => owners.set(mac, group)));
    const current = groups.find(group => group.id === selected);
    const members = current ? nodes.filter(node => current.node_macs.includes(node.mac)) : [];
    const onlineCount = group => nodes.filter(node => group.node_macs.includes(node.mac) && String(node.status).toLowerCase() === 'online').length;
    function editSampling(group) {
        setError(''); setNotice('');
        setSampling({ id: group.id, groupName: group.name, config: group.sampling_config ? { ...group.sampling_config } : {
            auto_interval_sec: 300, vibration_window_ms: 500, digital_mic_window_ms: 1000, analog_mic_window_ms: 1000,
            raw_upload_every_cycles: 1, vibration_enabled: true, digital_microphone_enabled: true, analog_microphone_enabled: true
        } });
    }
    async function applySampling(event) {
        event.preventDefault();
        setBusy(true); setError('');
        try {
            await api('/sampling', { ...sampling.config, id: sampling.id });
            setSampling(null);
            setNotice('组采样参数已保存并分配给全部当前成员；新成员将自动继承，后端将在节点通信时推送。');
            await load();
        } catch (problem) { setError(problem.message); }
        finally { setBusy(false); }
    }
    function edit(group) {
        setError(''); setSearch('');
        setDraft(group ? { ...group, node_macs: [...group.node_macs] } : { id: 0, name: '', description: '', node_macs: [] });
    }
    async function save(event) {
        event.preventDefault();
        const name = draft.name.trim();
        if (!name || new TextEncoder().encode(name).length > 128 || new TextEncoder().encode(draft.description).length > 768) {
            setError('组名必填且最多 128 字节，描述最多 768 字节（中文约占 3 字节）。'); return;
        }
        setBusy(true); setError('');
        try {
            await api('', { ...draft, name });
            setDraft(null);
            await load();
        } catch (problem) { setError(problem.message); }
        finally { setBusy(false); }
    }
    async function remove(group) {
        if (!window.confirm(`删除“${group.name}”（ID：${group.id}）？仅解除成员关系，不删除节点和历史数据。`)) return;
        setBusy(true); setError('');
        try {
            await api('/delete', { id: group.id });
            if (selected === group.id) setSelected(null);
            await load();
        } catch (problem) { setError(problem.message); }
        finally { setBusy(false); }
    }
    const candidates = nodes.filter(node => `${node.name} ${node.mac}`.toLowerCase().includes(search.toLowerCase()));
    return <section className="node-groups">
        <header className="group-toolbar group-toolbar-actions ui-toolbar">
            <div><button type="button" className="group-create-button ui-button ui-button-primary" disabled={busy || loading} onClick={() => edit(null)}><Plus size={16} aria-hidden="true" />创建分组</button></div></header>
        {error && !draft && !sampling && <p role="alert" className="group-error">{error}</p>}
        {notice && <p role="status">{notice}</p>}
        {selected !== null && !current && !loading && <p>该分组已不存在。<button onClick={() => setSelected(null)}>返回列表</button></p>}
        {current ? <>
            <header className="group-toolbar"><div><button onClick={() => setSelected(null)}>← 返回分组</button>
                <h3>{current.name} <small>ID：{current.id}</small></h3><p>{current.description || '暂无描述'}</p>
                <p>{current.node_macs.length} 个成员 · {onlineCount(current)} 个在线</p>
                <p>{current.sampling_applied ? `已应用组参数 · 周期 ${current.sampling_config.auto_interval_sec / 60} 分钟 · 振动 ${current.sampling_config.vibration_window_ms / 1000} 秒；新成员自动继承` : '尚未应用组参数，所有成员保留各自配置'}</p></div>
                <div><button disabled={busy} onClick={() => editSampling(current)}>采样参数</button>
                    <button disabled={busy} onClick={() => edit(current)}>编辑分组与成员</button></div></header>
            {members.length ? <div className="group-node-grid">{members.map((node, index) => <div key={node.mac}>
                {renderNode(node, index)}<div className="group-node-meta"><span>{node.mac} · RSSI {Number.isFinite(Number(node.rssi)) && Number(node.rssi) !== -128 ? `${node.rssi} dBm` : '--'}</span>
                <button onClick={() => onSettings(node)}>节点设置</button></div></div>)}</div> : <p>此分组暂无节点，点击“编辑分组与成员”添加。</p>}
        </> : selected === null && <>
            <GroupColumnSelector columns={columns} onChange={changeColumn} />
            <div className="group-table-scroll"><table><thead><tr>{GROUP_COLUMNS.filter(([key]) => columns[key]).map(([key, label]) => <th key={key}>{label}</th>)}</tr></thead>
            <tbody>{groups.map(group => <tr key={group.id}>
                {columns.name && <td><button className="group-link" onClick={() => setSelected(group.id)}>{group.name}</button></td>}
                {columns.id && <td>{group.id}</td>}{columns.description && <td className="group-description">{group.description || '—'}</td>}
                {columns.members && <td>{group.node_macs.length}</td>}{columns.online && <td>{onlineCount(group)}</td>}
                {columns.sampling && <td>{group.sampling_applied ? '已应用' : '未应用'}</td>}
                {columns.actions && <td><GroupActions disabled={busy} onSampling={() => editSampling(group)} onEdit={() => edit(group)} onDelete={() => remove(group)} /></td>}
            </tr>)}</tbody></table>
            {!groups.length && <p>{loading ? '正在加载…' : '暂无分组，点击“创建分组”开始。'}</p>}</div></>}
        {draft && <div className="group-modal-shade"><section className="group-modal" role="dialog" aria-modal="true" aria-labelledby="group-editor-title">
            <form onSubmit={save}><h3 id="group-editor-title">{draft.id ? `编辑分组 · ID：${draft.id}` : '创建分组 · ID 自动生成'}</h3>
                <label>组名（允许重复）<input autoFocus required value={draft.name} disabled={busy} onChange={event => setDraft({ ...draft, name: event.target.value })} /></label>
                <label>描述<textarea value={draft.description} disabled={busy} onChange={event => setDraft({ ...draft, description: event.target.value })} /></label>
                <label>选择成员 · 已选 {draft.node_macs.length} 个<input type="search" placeholder="搜索节点名称或 MAC" value={search} onChange={event => setSearch(event.target.value)} /></label>
                <div className="group-candidates">{candidates.map(node => {
                    const owner = owners.get(node.mac);
                    const unavailable = owner && owner.id !== draft.id;
                    return <label key={node.mac} className={unavailable ? 'unavailable' : ''}><input type="checkbox" disabled={busy || Boolean(unavailable)} checked={draft.node_macs.includes(node.mac)}
                        onChange={event => setDraft({ ...draft, node_macs: event.target.checked ? [...draft.node_macs, node.mac] : draft.node_macs.filter(mac => mac !== node.mac) })} />
                        <span>{node.name || node.mac}<small>{node.mac}</small>{unavailable && <small>已属于 {owner.name}（ID：{owner.id}），需先从原组移除</small>}</span></label>;
                })}{!candidates.length && <p>暂无匹配的已授权节点</p>}</div>
                <p>移除成员仅解除分组关系。离线或睡眠节点也可以保留在组内。</p>
                {draft.sampling_applied && <p>此组已应用采样参数：新添加的成员自动继承；原有成员配置不会因保存成员列表而重复覆盖。</p>}
                {error && <p role="alert" className="group-error">{error}</p>}
                <footer><button type="button" disabled={busy} onClick={() => setDraft(null)}>取消</button><button className="primary" disabled={busy || loading} type="submit">{busy ? '保存中…' : '保存'}</button></footer>
            </form></section></div>}
        {sampling && <div className="group-modal-shade" onClick={event => {
            if (event.target === event.currentTarget) cancelSampling();
        }}><section className="group-modal" role="dialog" aria-modal="true" aria-labelledby="group-sampling-title">
            <form onSubmit={applySampling}><h3 id="group-sampling-title">{sampling.groupName} · ID：{sampling.id} · 采样参数</h3>
                <p>点击应用才保存组参数并更新成员。仅打开或修改此窗口，不会改变节点配置。</p>
                <label>自动采集周期（分钟）<select disabled={busy} value={sampling.config.auto_interval_sec}
                    onChange={event => setSampling({ ...sampling, config: { ...sampling.config, auto_interval_sec: Number(event.target.value) } })}>
                    {[...new Set([60, 120, 180, 240, 300, 600, 1200, 1800, 3600, sampling.config.auto_interval_sec])].sort((a,b) => a-b).map(value => <option key={value} value={value}>{value / 60}</option>)}</select></label>
                {[
                    ['振动', 'vibration_enabled', 'vibration_window_ms', '26.667 kHz', [500, 1000, 1500]],
                    ['数字麦克风', 'digital_microphone_enabled', 'digital_mic_window_ms', '46.875 kHz', [500, 1000, 1500, 2000]],
                    ['超声麦克风', 'analog_microphone_enabled', 'analog_mic_window_ms', '256 kHz', [500, 1000, 1500, 2000]]
                ].map(([label, enabled, window, rate, values]) => <fieldset key={window} className="group-sampling-sensor"><legend>{label}</legend>
                    <label><input type="checkbox" disabled={busy} checked={sampling.config[enabled]}
                        onChange={event => setSampling({ ...sampling, config: { ...sampling.config, [enabled]: event.target.checked } })} /> 启用采集</label>
                    <p>采样率：{rate}（固定）</p><label>采集时长（秒）<select disabled={busy || !sampling.config[enabled]} value={sampling.config[window]}
                        onChange={event => setSampling({ ...sampling, config: { ...sampling.config, [window]: Number(event.target.value) } })}>
                        {values.map(value => <option key={value} value={value}>{value / 1000}</option>)}</select></label></fieldset>)}
                <label>原始波形上传频率（每 N 次采集）<input type="number" min="1" max="65535" required disabled={busy} value={sampling.config.raw_upload_every_cycles}
                    onChange={event => setSampling({ ...sampling, config: { ...sampling.config, raw_upload_every_cycles: event.target.value === '' ? '' : Number(event.target.value) } })} /></label>
                <p>不改变在线监测开关，不同步采样时间，也不主动唤醒节点；节点下次查询时取得参数。</p>
                {error && <p role="alert" className="group-error">{error}</p>}
                <footer><button type="button" disabled={busy} onClick={cancelSampling}>取消</button><button type="submit" className="primary" disabled={busy}>{busy ? '应用中…' : '应用到全组'}</button></footer>
            </form></section></div>}
    </section>;
}
