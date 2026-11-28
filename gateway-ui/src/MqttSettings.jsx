import React, { useEffect, useRef, useState } from 'react';
import { Activity, ArrowUpRight, Check, ChevronDown, CloudUpload, Eye, EyeOff, Info, LoaderCircle, PlugZap, Save, SlidersHorizontal, Wifi, WifiOff } from 'lucide-react';
import { mqttDraftFromConfig, validateMqttSettings, mqttSettingsPayload, MQTT_STATE_LABELS, formatMqttBytes, mqttRequest, waitForMqttTest } from './mqttSettings';

export default function MqttSettings() {
    const [draft, setDraft] = useState(null);
    const [saved, setSaved] = useState(null);
    const [config, setConfig] = useState(null);
    const [runtime, setRuntime] = useState(null);
    const [configValid, setConfigValid] = useState(true);
    const [loadError, setLoadError] = useState('');
    const [statusError, setStatusError] = useState('');
    const [reload, setReload] = useState(0);
    const [errors, setErrors] = useState({});
    const [showPassword, setShowPassword] = useState(false);
    const [busy, setBusy] = useState('');
    const [notice, setNotice] = useState('');
    const [operationError, setOperationError] = useState('');
    const operation = useRef(null);
    const generation = useRef(0);
    useEffect(() => {
        const controller = new AbortController();
        let polling = false;
        async function load() {
            try {
                const result = await mqttRequest('settings', { signal: controller.signal });
                if (controller.signal.aborted) return;
                const values = mqttDraftFromConfig(result.config);
                setDraft(values); setSaved(values); setConfig(result.config); setRuntime(result.runtime);
                setConfigValid(result.config_valid); setLoadError(''); setStatusError('');
            } catch (problem) { if (!controller.signal.aborted) setLoadError(problem.message || '无法读取 MQTT 配置'); }
        }
        async function poll() {
            if (polling || operation.current) return;
            polling = true; const version = generation.current;
            try {
                const result = await mqttRequest('status', { signal: controller.signal, timeout: 5000 });
                if (!controller.signal.aborted && version === generation.current) {
                    setRuntime(result.runtime); setConfigValid(result.config_valid); setStatusError('');
                }
            } catch (problem) {
                if (!controller.signal.aborted && version === generation.current) setStatusError(problem.message || '无法读取上传状态');
            } finally { polling = false; }
        }
        load(); const timer = window.setInterval(poll, 3000);
        return () => { controller.abort(); operation.current?.abort(); operation.current = null; window.clearInterval(timer); };
    }, [reload]);
    const dirty = draft && saved && Object.keys(draft).some(key => draft[key] !== saved[key]);
    const clientId = draft?.clientId || `gateway-${draft?.gatewayId || ''}`;
    const state = statusError ? 'unknown' : runtime?.state;
    const connected = state === 'connected';
    const failed = state === 'error' || Boolean(statusError);
    const status = MQTT_STATE_LABELS[state] || '状态未知';
    const update = (key, value) => {
        setDraft(previous => ({ ...previous, [key]: value }));
        setErrors(previous => ({ ...previous, [key]: undefined }));
        setNotice('');
        setOperationError('');
    };
    function validate(testing = false) {
        const next = validateMqttSettings(testing ? { ...draft, enabled: true } : draft);
        setErrors(next);
        if (Object.keys(next).length) {
            setOperationError('请先检查标红的配置项。');
            return false;
        }
        return true;
    }
    async function submit(testing, event) {
        event?.preventDefault();
        if (operation.current || !validate(testing)) return;
        const controller = new AbortController(); operation.current = controller; generation.current++;
        setBusy(testing ? 'testing' : 'saving'); setOperationError(''); setNotice('');
        try {
            const body = mqttSettingsPayload(draft, config);
            let result = await mqttRequest(testing ? 'test' : 'settings', { signal: controller.signal, body });
            if (testing) {
                const deadline = Date.now() + (config.connect_timeout_ms || 10000) + 8000;
                while (result.state === 'testing') {
                    if (Date.now() >= deadline) throw new Error('连接测试未及时返回，请稍后重试。');
                    await waitForMqttTest(controller.signal);
                    result = await mqttRequest(`test?id=${encodeURIComponent(result.id)}`, { signal: controller.signal, timeout: 5000 });
                }
                if (result.state !== 'success') throw new Error(result.message || '连接测试失败');
            } else if (!controller.signal.aborted) {
                const values = mqttDraftFromConfig(result.config);
                setDraft(values); setSaved(values); setConfig(result.config); setRuntime(result.runtime);
                setConfigValid(result.config_valid); setStatusError(''); setShowPassword(false);
                setNotice(result.config.enabled ? '' : '配置已保存，MQTT 上传已停用，本地采集数据继续保留。');
            }
        } catch (problem) { if (!controller.signal.aborted) setOperationError(problem.message || '操作失败，请检查后端连接。'); }
        finally {
            if (!controller.signal.aborted) { operation.current = null; setBusy(''); }
        }
    }
    const field = (name, label, options = {}) => <label className={`mqtt-field${options.wide ? ' mqtt-field-wide' : ''}`}>
        <span>{label}{options.optional && <small>可选</small>}</span>
        <div className="mqtt-input-wrap">
            <input name={name} value={draft[name]} onChange={event => update(name, event.target.value)}
                type={options.type || 'text'} placeholder={options.placeholder}
                disabled={Boolean(busy) || options.disabled}
                inputMode={options.numeric ? 'numeric' : undefined} autoComplete={name === 'password' ? 'new-password' : 'off'}
                aria-invalid={Boolean(errors[name])} aria-describedby={errors[name] ? `mqtt-error-${name}` : undefined} />
            {name === 'password' && <button type="button" className="mqtt-password-eye" disabled={Boolean(busy) || options.disabled}
                onClick={() => setShowPassword(value => !value)} aria-label={showPassword ? '隐藏密码' : '显示密码'}>
                {showPassword ? <EyeOff size={16} /> : <Eye size={16} />}
            </button>}
        </div>
        {errors[name] ? <small className="mqtt-field-error" id={`mqtt-error-${name}`}>{errors[name]}</small>
            : options.hint && <small className="mqtt-field-hint">{options.hint}</small>}
    </label>;

    if (!draft) return <section className="mqtt-panel mqtt-load-state" aria-busy={!loadError}>
        {loadError ? <><p role="alert">{loadError}</p><button type="button" className="mqtt-button" onClick={() => { setLoadError(''); setReload(value => value + 1); }}>重新加载</button></>
            : <p role="status"><LoaderCircle size={17} className="mqtt-spin" />正在读取 MQTT 配置…</p>}
    </section>;
    const bytes = formatMqttBytes(statusError ? null : runtime?.pending_bytes);
    const confirmed = !statusError && runtime?.last_confirmed_at ? new Date(runtime.last_confirmed_at * 1000) : null;
    return <div className="mqtt-settings">
        <section className="mqtt-panel mqtt-upload-status" aria-label="上传状态">
            <header><Activity size={16} /><h3>上传状态</h3><span>每 3 秒自动更新</span></header>
            {failed && <div className="mqtt-failure" role="alert">{statusError || runtime?.error_message || '连接或上传队列异常，请检查网关日志。'}</div>}
            {!configValid && <p className="mqtt-failure" role="alert">后端配置文件无效或无法读取，MQTT 未正常加载；请检查并保存有效配置。</p>}
            <div className="mqtt-stats">
                <div><span>连接状态</span><strong className={connected ? 'mqtt-text-success' : ''}>{connected ? <Wifi size={18} /> : <WifiOff size={18} />}{status}</strong></div>
                <div><span>待上传批次总数</span><strong>{!statusError && runtime?.pending_batches != null ? runtime.pending_batches : '—'}<small>批</small></strong></div>
                <div><span>待上传数据量</span><strong>{bytes.value}<small>{bytes.unit}</small></strong></div>
                <div><span>最近发送完成</span><strong className="mqtt-time">{confirmed ? confirmed.toLocaleTimeString('zh-CN', { hour12: false }) : '—'}<small>{confirmed ? confirmed.toLocaleDateString('zh-CN') : statusError ? '状态不可用' : '暂无记录'}</small></strong></div>
            </div>
            {!statusError && runtime && !runtime.queue_available && <p className="mqtt-runtime-note">上传队列统计不可用，请检查数据库；不会显示为零或上传成功。</p>}
            {!statusError && runtime?.other_pending_batches > 0 && <p className="mqtt-runtime-note">另有 {runtime.other_pending_batches} 批属于其他服务器、账号或网关标识，仍保留在本地，未转发到当前目标。</p>}
        </section>
        <form onSubmit={event => submit(false, event)} noValidate>
            <section className="mqtt-panel mqtt-connection" aria-labelledby="mqtt-connection-heading">
                <header className="mqtt-panel-heading">
                    <div className="mqtt-heading-icon"><CloudUpload size={21} strokeWidth={1.7} /></div>
                    <div><h2 id="mqtt-connection-heading">MQTT 连接</h2><p>通过 Sparkplug B 发布采集结果与完整原始数据</p></div>
                    <span className={`mqtt-status${connected ? ' is-connected' : failed ? ' is-failed' : ''}`}>
                        {state === 'connecting' ? <LoaderCircle size={13} className="mqtt-spin" /> : <span />}{status}
                    </span>
                    <label className="mqtt-enable"><span>启用上传</span><input type="checkbox" role="switch"
                        checked={draft.enabled} disabled={Boolean(busy)} onChange={event => update('enabled', event.target.checked)} /></label>
                </header>
                <div className="mqtt-layout">
                    <div className="mqtt-config">
                        <div className="mqtt-section-label"><PlugZap size={15} /><h3>服务器与认证</h3><span className="mqtt-protocol">MQTT / TCP</span></div>
                        <div className="mqtt-fields mqtt-server-fields">
                            {field('host', '服务器地址', { placeholder: 'IP 地址或域名' })}
                            {field('port', '端口', { numeric: true })}
                        </div>
                        <div className="mqtt-fields">
                            {field('groupId', '分组 ID', { wide: true, hint: '同一站点或产线使用同一个分组 ID' })}
                            {field('username', '用户名', { optional: true, placeholder: '服务器认证用户名', disabled: config.username_from_env, hint: config.username_from_env ? '由后端环境变量管理' : '' })}
                            {field('password', '密码', { optional: true, type: showPassword ? 'text' : 'password',
                                disabled: config.password_from_env,
                                placeholder: config.password_from_env ? '密码由后端环境变量管理' : '请输入密码（可选）' })}
                            {field('gatewayId', '网关 ID', { wide: true, hint: '用于区分不同网关的采集数据' })}
                        </div>
                        <details className="mqtt-advanced">
                            <summary><SlidersHorizontal size={15} /><span>高级配置</span><ChevronDown size={15} /></summary>
                            <div className="mqtt-fields">
                                {field('clientId', '客户端 ID', { wide: true, optional: true, placeholder: clientId, hint: '留空时按网关 ID 自动生成，同一服务器上的客户端 ID 必须唯一' })}
                                {field('keepalive', '心跳间隔（秒）', { numeric: true })}
                                {field('ackTimeout', '发送超时（秒）', { numeric: true })}
                            </div>
                        </details>
                        <p className="mqtt-security-note"><Info size={14} />当前使用未加密连接，仅用于可信网络；暂不提供 TLS 配置。</p>
                    </div>
                    <aside className="mqtt-delivery" aria-label="数据发布配置">
                        <div className="mqtt-section-label"><ArrowUpRight size={15} /><h3>数据发布</h3></div>
                        <dl className="mqtt-publish-properties">
                            <div><dt>上传协议</dt><dd>Sparkplug B / MQTT 3.1.1</dd></div>
                            <div><dt>数据发送等级</dt><dd>QoS 0</dd></div>
                            <div><dt>批次数据格式</dt><dd>Protobuf / DDATA</dd></div>
                            <div><dt>原始振动数据</dt><dd>完整XYZ三轴波形，int16小端格式</dd></div>
                            <div><dt>保留消息</dt><dd>关闭</dd></div>
                        </dl>
                    </aside>
                </div>
                <footer className="mqtt-actions">
                    <span className="mqtt-draft-state">{dirty ? <><span className="mqtt-unsaved-dot" />有未保存的修改</> : <><Check size={14} />与已保存配置一致</>}</span>
                    <div><button type="button" className="mqtt-button" onClick={() => submit(true)} disabled={Boolean(busy)}>
                        {busy === 'testing' ? <LoaderCircle size={15} className="mqtt-spin" /> : <PlugZap size={15} />}{busy === 'testing' ? '测试中…' : '测试连接'}
                    </button><button type="submit" className="mqtt-button mqtt-button-primary" disabled={Boolean(busy)}>
                        <Save size={15} />{busy === 'saving' ? '正在保存…' : '保存并应用'}
                    </button></div>
                </footer>
            </section>
        </form>
        <div className="mqtt-notice" role="status" aria-live="polite">{notice}</div>
        {operationError && <p className="mqtt-failure" role="alert">{operationError}</p>}
    </div>;
}
