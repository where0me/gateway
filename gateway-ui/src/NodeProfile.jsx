import React, { useState } from 'react';
import { ArrowLeft, CircuitBoard, Info, Settings, Pencil, Check, X } from 'lucide-react';
import { nodeLastSeen, relativeSeen, networkAddress } from './nodeManagementColumns.js';
import { handleProfileEditorKeyDown } from './nodeProfileKeyboard.js';

function ProfileEditor({ value, label, multiline, onSave, disabled, onEditing, children }) {
    const [editing, setEditing] = useState(false);
    const [draft, setDraft] = useState('');
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState('');
    const close = () => { setEditing(false); setError(''); onEditing(false); };
    const submit = async event => {
        event.preventDefault();
        if (busy) return;
        if (multiline && new TextEncoder().encode(draft).length > 768) {
            setError('描述过长，最多支持 768 个 UTF-8 字节（约 256 个汉字）'); return;
        }
        setBusy(true); setError('');
        try { await onSave(draft); close(); } catch (err) { setError(err.message || '保存失败'); }
        finally { setBusy(false); }
    };
    if (!editing) {
        const button = <button type="button" className="node-profile-edit" title={label} aria-label={label} disabled={disabled}
            onClick={() => { setDraft(value || ''); setEditing(true); onEditing(true); }}><Pencil size={15} /></button>;
        return multiline ? <div className="node-profile-description-display"><span>{children}</span>{button}</div>
            : <>{children}{button}</>;
    }
    const Input = multiline ? 'textarea' : 'input';
    return <form className={`node-profile-editor${multiline ? ' node-profile-editor-description' : ''}`} onSubmit={submit}>
        <Input autoFocus value={draft} onChange={event => setDraft(event.target.value)} aria-label={label}
            onKeyDown={event => handleProfileEditorKeyDown(event, busy)}
            maxLength={multiline ? 768 : 32} disabled={busy} rows={multiline ? 3 : undefined} />
        <button type="submit" title="保存" aria-label={`保存${label}`} disabled={busy}><Check size={16} /></button>
        <button type="button" title="取消" aria-label={`取消${label}`} onClick={close} disabled={busy}><X size={16} /></button>
        <small>{multiline ? 'Enter 保存，Shift+Enter 换行；清空后恢复默认描述。' : '清空后恢复默认名称。'}</small>
        {error && <span role="alert" className="compact-error">{error}</span>}
    </form>;
}

export default function NodeProfile({ node, nodes = [node], onSelect, onRename, onDescription, observed, now, saving, onBack, children }) {
    const [tab, setTab] = useState('settings');
    const [imageFailed, setImageFailed] = useState(false);
    const [editing, setEditing] = useState(false);
    const blocked = saving || editing;
    const online = String(node.status).toLowerCase() === 'online';
    const address = networkAddress(node.short_addr);
    const time = nodeLastSeen(node, observed);
    const description = node.description || (node.model_name === 'HY-Sensor'
        ? '多传感器监测节点，支持温度、磁场、振动和声级采集。' : '暂无节点描述');
    const tabs = [['about', '关于', Info], ['settings', '设置', Settings]];
    return <div className="node-profile">
        <header className="node-profile-heading">
            <button className="node-profile-back" type="button" onClick={onBack} disabled={blocked}><ArrowLeft size={16} />返回节点管理</button>
            <select className="node-profile-selector" aria-label="切换节点" value={node.mac} disabled={blocked || !onSelect}
                onChange={event => {
                    const selected = nodes.find(item => item.mac === event.target.value);
                    if (selected && selected.mac !== node.mac && window.confirm('切换节点会重新读取采样配置，未保存的采样参数将丢弃。是否继续？')) {
                        setImageFailed(false); onSelect(selected);
                    }
                }}>
                {nodes.map(item => <option key={item.mac} value={item.mac}>{item.name || item.mac}</option>)}
            </select>
        </header>
        <div className="node-profile-body">
            <nav className="node-profile-tabs" role="tablist" aria-label="节点信息" onKeyDown={event => {
                if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
                event.preventDefault();
                const next = event.key === 'Home' ? 'about' : event.key === 'End' ? 'settings' : tab === 'about' ? 'settings' : 'about';
                setTab(next);
                event.currentTarget.querySelector(`#node-tab-${next}`).focus();
            }}>
                {tabs.map(([id, label, Icon]) => <button key={id} id={`node-tab-${id}`} type="button" role="tab"
                    aria-selected={tab === id} aria-controls={`node-panel-${id}`} tabIndex={tab === id ? 0 : -1}
                    className={tab === id ? 'is-active' : ''} onClick={() => setTab(id)}><Icon size={16} aria-hidden="true" />{label}</button>)}
            </nav>
            <section id="node-panel-about" role="tabpanel" aria-labelledby="node-tab-about" hidden={tab !== 'about'}>
                <div className="node-profile-about">
                    <figure className="node-profile-image">
                        {node.image_url && !imageFailed ? <img src={node.image_url} alt={`${node.name || node.model_name} 节点图片`} onError={() => setImageFailed(true)} />
                            : <><CircuitBoard size={100} strokeWidth={1} aria-hidden="true" /><figcaption>暂无节点图片</figcaption></>}
                    </figure>
                    <div className="node-profile-info">
                        <div className="node-profile-name">
                            {onRename ? <ProfileEditor key={`name-${node.mac}`} value={node.name || node.mac} label="重命名" disabled={blocked}
                                onEditing={setEditing} onSave={value => onRename(node.mac, value)}><h3>{node.name || node.mac}</h3></ProfileEditor>
                                : <h3>{node.name || node.mac}</h3>}
                            <span className={`node-table-availability${online ? ' is-online' : ''}`}>● {online ? 'Online' : 'Offline'}</span></div>
                        <dl>
                            <dt>描述</dt><dd className="node-profile-description">
                                {onDescription ? <ProfileEditor key={`description-${node.mac}`} value={description} label="自定义描述" multiline disabled={blocked}
                                    onEditing={setEditing} onSave={value => onDescription(node.mac, value)}>{description}</ProfileEditor> : description}</dd>
                            <dt>MAC地址</dt><dd className="node-profile-address">{node.mac || '未上报'}</dd>
                            <dt>网络地址</dt><dd>{address ? <><strong>{address.hex}</strong><small>网络地址（十进制）：{address.decimal}</small>{!online && <small>离线前记录的地址</small>}</> : '未上报'}</dd>
                            <dt>最后出现</dt><dd><time dateTime={time ? new Date(time).toISOString() : undefined}>{relativeSeen(time, now)}</time>{time > 0 && <small>{new Date(time).toLocaleString('zh-CN', { hour12: false })}</small>}</dd>
                        </dl>
                    </div>
                </div>
            </section>
            <section id="node-panel-settings" role="tabpanel" aria-labelledby="node-tab-settings" hidden={tab !== 'settings'}>
                <div className="node-profile-notice" role="note">
                    <Info size={18} aria-hidden="true" />
                    <p><strong>配置生效方式</strong>当前配置仅属于此节点。保存到网关后，节点在唤醒或上传结束查询时接收；节点休眠时不会立即生效，当前采集及上传不会被打断。</p>
                </div>
                {children}
            </section>
        </div>
    </div>;
}
