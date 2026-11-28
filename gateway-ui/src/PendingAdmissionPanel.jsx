import React, { useEffect, useRef, useState } from 'react';
import { Check, LoaderCircle, Network, RefreshCw, Signal } from 'lucide-react';
import './PendingAdmissionPanel.css';

export function PendingAdmissionTable({ nodes, saving = '', error = '', onAuthorize, onRefresh, refreshing = false }) {
    return <section className="pending-admission" aria-labelledby="pending-admission-title">
        <header className="pending-admission-heading">
            <span className="pending-admission-icon"><Network size={19} aria-hidden="true" /></span>
            <h3 id="pending-admission-title">待授权列表</h3>
            <span className="pending-admission-count" aria-label={`${nodes.length}个待授权节点`}>{nodes.length}</span>
            <button type="button" className="pending-admission-refresh" onClick={onRefresh}
                disabled={refreshing || Boolean(saving)} aria-busy={refreshing}>
                <RefreshCw size={14} className={refreshing ? 'pending-admission-spinner' : undefined} aria-hidden="true" />
                {refreshing ? '刷新中…' : '刷新'}
            </button>
        </header>
        {error && <p role="alert" className="pending-admission-error">{error}</p>}
        <div className="pending-admission-scroll"><table className="pending-admission-table">
            <thead><tr>{['设备名称', '型号', 'MAC', 'LQI', 'RSSI', '操作'].map(label => <th key={label} scope="col">{label}</th>)}</tr></thead>
            <tbody>{nodes.length ? nodes.map(node => <tr key={node.mac}>
                <td><strong className="pending-admission-name">{node.name}</strong></td>
                <td className="pending-admission-model">{node.model_name}</td>
                <td><span className="pending-admission-mac">{node.mac}</span></td>
                <td><span className={`pending-admission-signal${node.lqi < 50 ? ' is-low' : ''}`}>
                    <Signal size={15} aria-hidden="true" />{node.lqi}</span></td>
                <td className="pending-admission-rssi">{node.rssi}<span> dBm</span></td>
                <td><button type="button" className="pending-admission-confirm" disabled={Boolean(saving) || node.authorizing}
                    onClick={() => onAuthorize(node)}>
                    {saving === node.mac || node.authorizing ? <LoaderCircle size={15} className="pending-admission-spinner" aria-hidden="true" /> : <Check size={15} aria-hidden="true" />}
                    {saving === node.mac || node.authorizing ? '确认中…' : '确认入网'}
                </button></td>
            </tr>) : <tr className="pending-admission-empty"><td colSpan={6} /></tr>}</tbody>
        </table></div>
    </section>;
}

export default function PendingAdmissionPanel({ onAuthorized }) {
    const [nodes, setNodes] = useState([]);
    const [saving, setSaving] = useState('');
    const [error, setError] = useState('');
    const [refreshVersion, setRefreshVersion] = useState(0);
    const [refreshing, setRefreshing] = useState(false);
    const pendingAuthorizations = useRef(new Set());
    const endpoint = `http://${window.location.hostname}:8080/api/device`;
    useEffect(() => {
        const controller = new AbortController();
        let cancelled = false, timer;
        const poll = async () => {
            try {
                const response = await fetch(`${endpoint}/pending`, { signal: controller.signal, cache: 'no-store' });
                if (!response.ok) throw new Error('读取待授权节点失败');
                const result = await response.json();
                if (!cancelled && Array.isArray(result)) {
                    const remaining = new Set(result.filter(node => node.authorizing).map(node => node.mac));
                    const changed = [...pendingAuthorizations.current].some(mac => !remaining.has(mac));
                    pendingAuthorizations.current = remaining;
                    setNodes(result);
                    setError('');
                    // A candidate may also disappear when hidden. Reload the
                    // authoritative device list instead of promoting it here.
                    if (changed) onAuthorized();
                }
            } catch (problem) {
                if (!cancelled) { setNodes([]); setError(problem.message); }
            } finally {
                if (!cancelled) {
                    timer = setTimeout(poll, pendingAuthorizations.current.size ? 100 : 2000);
                }
            }
        };
        poll();
        return () => { cancelled = true; controller.abort(); clearTimeout(timer); };
    }, [endpoint, onAuthorized, refreshVersion]);
    const refresh = async () => {
        setRefreshing(true); setError('');
        try {
            const response = await fetch(`${endpoint}/pending/refresh`, { method: 'POST' });
            const result = await response.json();
            if (!response.ok) throw new Error(result.msg || '发起信号探测失败');
            // Start a real radio-probe round, then keep polling for its replies.
            setRefreshVersion(previous => previous + 1);
        } catch (problem) { setError(problem.message); }
        finally { setRefreshing(false); }
    };
    const authorize = async node => {
        setSaving(node.mac); setError('');
        try {
            const response = await fetch(`${endpoint}/authorize`, {
                method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ mac: node.mac })
            });
            const result = await response.json();
            if (!response.ok || result.status !== 'success') throw new Error(result.msg || '确认入网失败');
            if (result.authorized) {
                pendingAuthorizations.current.delete(node.mac);
                setNodes(previous => previous.filter(item => item.mac !== node.mac));
                onAuthorized();
            } else {
                pendingAuthorizations.current.add(node.mac);
                setNodes(previous => previous.map(item => item.mac === node.mac ? { ...item, authorizing: true } : item));
                setRefreshVersion(previous => previous + 1);
            }
        } catch (problem) { setError(problem.message); }
        finally { setSaving(''); }
    };
    return <PendingAdmissionTable nodes={nodes} saving={saving} error={error} onAuthorize={authorize}
        onRefresh={refresh} refreshing={refreshing} />;
}
