import React, { useCallback, useEffect, useRef, useState } from 'react';
import { Download, RefreshCw, Search, Trash2, X } from 'lucide-react';
import { networkAddress } from './nodeManagementColumns.js';
import { LOG_SOURCES as SOURCES, LOG_LEVELS as LEVELS, LOG_EVENTS as EVENTS, logsCsv, logParameters, readLogClearBoundary, saveLogClearBoundary } from './gatewayLogsFormat.js';
async function readLogs(filters, signal) {
    const response = await fetch(`http://${window.location.hostname}:8080/api/logs?${logParameters(filters)}`, { signal });
    const result = await response.json();
    if (!response.ok || result.status !== 'success' || !Array.isArray(result.items)) throw new Error(result.msg || '读取日志失败');
    return { ...result, items: result.items.slice(0, Number(filters.limit || 100)) };
}
function LogFilters({ filters, nodes, onChange, showTimes = true }) {
    return <>
        <label>等级<select name="level" value={filters.level} onChange={onChange}><option value="">全部等级</option>{Object.entries(LEVELS).map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label>
        <label>来源<select name="source" value={filters.source} onChange={onChange}><option value="">全部来源</option>{Object.entries(SOURCES).map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select></label>
        <label>节点<select name="mac" value={filters.mac} onChange={onChange}><option value="">全部节点</option>{nodes.map(node => <option key={node.mac} value={node.mac}>{node.name || node.mac}</option>)}</select></label>
        {showTimes && <label>开始时间<input type="datetime-local" name="from" value={filters.from} onChange={onChange} /></label>}
        {showTimes && <label>结束时间<input type="datetime-local" name="to" value={filters.to} onChange={onChange} /></label>}
    </>;
}
function LogDialog({ mode, filters, nodes, onClose, onQuery }) {
    const dialog = useRef(null);
    const request = useRef(null);
    const [draft, setDraft] = useState(filters);
    const [data, setData] = useState({ items: [], total: 0 });
    const [busy, setBusy] = useState(mode === 'query');
    const [error, setError] = useState('');
    const [note, setNote] = useState('');
    const exporting = mode === 'export';
    const configuring = mode !== 'query';
    const load = useCallback(async (selected, download = false) => {
        request.current?.abort();
        const controller = new AbortController(); request.current = controller;
        setBusy(true); setError(''); setNote(''); setData({ items: [], total: 0 });
        try {
            const result = await readLogs(selected, controller.signal);
            if (controller.signal.aborted) return;
            setData(result);
            if (download) {
                if (!result.items.length) { setNote('没有符合条件的日志，未生成导出文件。'); return; }
                const url = URL.createObjectURL(new Blob([logsCsv(result.items)], { type: 'text/csv;charset=utf-8' }));
                const link = document.createElement('a'); link.href = url; link.download = `网关日志-${new Date().toISOString().slice(0, 10)}.csv`;
                link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
                setNote(`已导出 ${result.items.length} 条日志${result.total > 100 ? '（匹配超过 100 条，仅导出最新 100 条）' : ''}。`);
            }
        } catch (err) { if (!controller.signal.aborted) setError(err.message || '读取日志失败'); }
        finally { if (!controller.signal.aborted) setBusy(false); }
    }, []);
    useEffect(() => {
        let active = true;
        dialog.current.showModal();
        if (mode === 'query') Promise.resolve().then(() => { if (active) load(filters); });
        return () => { active = false; request.current?.abort(); };
    }, [mode, filters, load]);
    const submit = event => {
        event.preventDefault();
        if (exporting) { load(draft, true); return; }
        try { logParameters(draft); setError(''); onQuery({ ...draft }); }
        catch (err) { setError(err.message); }
    };
    return <dialog ref={dialog} className={`logs-dialog ${configuring ? 'logs-export-dialog' : ''}`} aria-labelledby="logs-dialog-title" onCancel={onClose}>
        <header className="logs-heading"><h2 id="logs-dialog-title">{exporting ? '导出日志' : configuring ? '查询日志' : '查询日志结果'}</h2><button type="button" onClick={onClose} aria-label="关闭窗口" title="关闭"><X size={18} /></button></header>
        <p className="logs-note">仅限后端本次启动的日志，{exporting ? '导出' : '显示'}符合条件的最新 100 条。</p>
        {configuring && <form className="logs-filters" onSubmit={submit}>
            <LogFilters filters={draft} nodes={nodes} onChange={event => setDraft(previous => ({ ...previous, [event.target.name]: event.target.value }))} />
            <button type="submit" disabled={busy}>{exporting ? <Download size={15} /> : <Search size={15} />}{exporting ? busy ? '正在导出…' : '确认导出' : '确认查询'}</button>
        </form>}
        {error && <p role="alert" className="compact-error">{error}</p>}
        {note && <p role="status" className="logs-note">{note}</p>}
        {!configuring && <><LogsConsole data={data} nodes={nodes} loading={busy} error={error} /><p className="logs-note">共匹配 {data.total} 条，显示 {data.items.length} 条。查询结果不自动刷新。</p></>}
    </dialog>;
}

export default function GatewayLogs({ nodes = [] }) {
    const [filter, setFilter] = useState({ level: '', source: '', mac: '' });
    const [limit, setLimit] = useState(100);
    const [hiddenThrough, setHiddenThrough] = useState(() => readLogClearBoundary());
    const [popup, setPopup] = useState(null);
    const [revision, setRevision] = useState(0);
    const [data, setData] = useState({ items: [], total: 0, dropped: 0 });
    const [loading, setLoading] = useState(true);
    const [error, setError] = useState('');
    useEffect(() => {
        let stopped = false, inFlight = false;
        const controller = new AbortController();
        const load = async () => {
            if (inFlight) return;
            inFlight = true;
            try {
                const result = await readLogs({ ...filter, limit }, controller.signal);
                if (!stopped) { setData(result); setError(''); }
            } catch (err) { if (!stopped) { setData({ items: [], total: 0, dropped: 0 }); setError(err.message || '读取日志失败'); } }
            finally { inFlight = false; if (!stopped) setLoading(false); }
        };
        load();
        const timer = setInterval(load, 2000);
        return () => { stopped = true; controller.abort(); clearInterval(timer); };
    }, [filter, limit, revision]);
    const change = event => {
        setLoading(true); setData({ items: [], total: 0, dropped: 0 });
        setFilter(previous => ({ ...previous, [event.target.name]: event.target.value }));
    };
    const openConfiguration = mode => setPopup({ mode, filters: { ...filter, from: '', to: '' } });
    const visibleData = { ...data, items: data.items.filter(row => row.id > hiddenThrough).slice(0, limit) };
    const clearDisplay = () => {
        const boundary = Math.max(hiddenThrough, ...data.items.map(row => row.id));
        saveLogClearBoundary(boundary);
        setHiddenThrough(boundary);
    };
    return <section className="gateway-logs">
        <div className="logs-filters" role="group" aria-label="实时日志筛选">
            <LogFilters filters={filter} nodes={nodes} onChange={change} showTimes={false} />
            <div className="logs-display-tools ui-toolbar">
                <label>日志上限<select aria-label="日志上限" value={limit} onChange={event => setLimit(Number(event.target.value))}>{[100, 200, 500, 1000].map(value => <option key={value} value={value}>{value}</option>)}</select></label>
                <button type="button" className="logs-clear-button" onClick={clearDisplay} title="仅清空当前显示，数据库日志保留"><Trash2 size={15} />清空</button>
                <button type="button" onClick={() => openConfiguration('query-config')}><Search size={15} />查询日志</button>
                <button type="button" onClick={() => openConfiguration('export')}><Download size={15} />导出日志</button>
                <button type="button" onClick={() => setRevision(value => value + 1)}><RefreshCw size={15} />刷新</button>
            </div></div>
        {data.dropped > 0 && <p role="alert" className="logs-note">本次后端运行中，日志缓冲满时丢弃了 {data.dropped} 条记录。</p>}
        {error && <p role="alert" className="compact-error">{error}，请确认后端已更新并运行。</p>}
        <LogsConsole data={visibleData} nodes={nodes} loading={loading} error={error} />
        <footer className="logs-pagination"><span>显示 {visibleData.items.length} 条 · 最多 {limit} 条</span></footer>
        {popup && <LogDialog key={popup.mode} mode={popup.mode} filters={popup.filters} nodes={nodes} onClose={() => setPopup(null)} onQuery={filters => setPopup({ mode: 'query', filters })} />}
    </section>;
}

export function LogsConsole({ data, nodes, loading, error }) {
    return <div className="logs-console" role="region" aria-label="日志窗口" tabIndex={0} aria-busy={loading}>
            <ol className="logs-console-lines">{data.items.map((row, index) => <li key={row.id} className={`logs-console-line log-${String(row.level).toLowerCase()}`}>
                <span className="logs-line-number" aria-hidden="true">{index + 1}</span>
                <span className="logs-line-text"><time dateTime={new Date(row.timestamp * 1000).toISOString()}>[{new Date(row.timestamp * 1000).toLocaleString('zh-CN', { hour12: false })}]</time>
                    {' '}[{row.level}] {SOURCES[row.source] || row.source}: {EVENTS[row.event] || row.event}
                    {row.mac && ` | 节点=${nodes.find(node => node.mac === row.mac)?.name || row.mac} | MAC=${row.mac}`}
                    {networkAddress(row.short_addr) && ` | 网络地址=${networkAddress(row.short_addr).hex}`}
                    {row.batch != null && ` | 批次=${row.batch}`}
                    {' | '}{row.message}
                </span>
            </li>)}</ol>
            {!data.items.length && <p className="logs-console-empty">{loading ? '正在读取日志…' : error ? '日志暂不可用' : '暂无符合条件的日志'}</p>}
        </div>;
}
