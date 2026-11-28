export function mqttDraftFromConfig(config) {
    const url = config.url ? new URL(config.url) : null;
    return {
        enabled: Boolean(config.enabled), host: url?.hostname || '', port: url?.port || '1883',
        username: config.username || '', password: config.password_from_env ? '' : config.password || '',
        gatewayId: config.gateway_id, clientId: config.client_id || '',
        groupId: config.group_id || 'factory01',
        keepalive: String(config.keepalive_sec), ackTimeout: String(config.ack_timeout_ms / 1000)
    };
}

export function validateMqttSettings(values) {
    const errors = {};
    if ((values.enabled && !values.host) || /[\s/#?@\\]/.test(values.host) || values.host.includes('://'))
        errors.host = '请输入服务器 IP 或域名，不包含协议、端口或路径。';
    else if (values.host.includes(':') && !(values.host.startsWith('[') && values.host.endsWith(']')))
        errors.host = '端口请填在右侧；IPv6 地址请使用方括号包裹。';
    const integerInRange = (value, min, max) => /^\d+$/.test(value) && Number(value) >= min && Number(value) <= max;
    if (!integerInRange(values.port, 1, 65535)) errors.port = '端口范围为 1～65535。';
    if (!/^[A-Za-z0-9_.-]{1,63}$/.test(values.gatewayId)) errors.gatewayId = '使用 1～63 位字母、数字、点、短横线或下划线。';
    if (!/^[A-Za-z0-9_.-]{1,63}$/.test(values.groupId)) errors.groupId = '分组 ID 使用 1～63 位字母、数字、点、短横线或下划线。';
    if (values.clientId && !/^[A-Za-z0-9_.-]{1,127}$/.test(values.clientId)) errors.clientId = '客户端 ID 最多 127 位，仅使用字母、数字、点、短横线或下划线。';
    if (!integerInRange(values.keepalive, 5, 3600)) errors.keepalive = '请输入 5～3600 秒。';
    if (!integerInRange(values.ackTimeout, 1, 300)) errors.ackTimeout = '请输入 1～300 秒。';
    const bytes = value => new TextEncoder().encode(value).length;
    if (bytes(values.username) > 127 || values.username.includes('\0')) errors.username = '用户名最多 127 字节，不能包含空字符。';
    if (bytes(values.password) > 255 || values.password.includes('\0')) errors.password = '密码最多 255 字节，不能包含空字符。';
    return errors;
}

export function mqttSettingsPayload(draft, config) {
    const payload = {
        enabled: draft.enabled, url: draft.host ? `mqtt://${draft.host}:${draft.port}` : '',
        gateway_id: draft.gatewayId, client_id: draft.clientId,
        protocol: 'sparkplug_b', group_id: draft.groupId,
        keepalive_sec: Number(draft.keepalive), ack_timeout_ms: Number(draft.ackTimeout) * 1000
    };
    if (!config.username_from_env) payload.username = draft.username;
    if (!config.password_from_env) {
        if (draft.password) payload.password = draft.password;
        else if (config.password_set) payload.clear_password = true;
    }
    return payload;
}

export const MQTT_STATE_LABELS = { disabled: '未启用', connected: '已连接', connecting: '连接中', disconnected: '未连接', error: '连接异常' };
export function formatMqttBytes(value) {
    if (value == null || !Number.isFinite(value)) return { value: '—', unit: '' };
    const units = ['B', 'KB', 'MB', 'GB']; let unit = 0, size = value;
    while (size >= 1024 && unit < units.length - 1) { size /= 1024; unit++; }
    return { value: unit ? size.toFixed(1) : String(size), unit: units[unit] };
}

export async function mqttRequest(path, { signal, body, timeout = 12000 } = {}) {
    const controller = new AbortController();
    const abort = () => controller.abort();
    if (signal?.aborted) controller.abort();
    else signal?.addEventListener('abort', abort, { once: true });
    const timer = setTimeout(abort, timeout);
    try {
        const host = window.location.hostname;
        const authority = host.includes(':') && !host.startsWith('[') ? `[${host}]` : host;
        const response = await fetch(`http://${authority}:8080/api/mqtt/${path}`, {
            method: body === undefined ? 'GET' : 'POST', signal: controller.signal,
            headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
            body: body === undefined ? undefined : JSON.stringify(body), cache: 'no-store'
        });
        const result = await response.json();
        if (!response.ok || result.status !== 'success') throw new Error(result.msg || 'MQTT 设置请求失败');
        return result;
    } catch (problem) {
        if (controller.signal.aborted && !signal?.aborted) throw new Error('后端请求超时，请检查网关连接。');
        if (!signal?.aborted && problem instanceof TypeError) throw new Error('无法连接后端，请确认网关服务已启动且 8080 端口可访问。');
        if (!signal?.aborted && problem instanceof SyntaxError) throw new Error('后端响应格式无效，请确认已更新并启动新版后端。');
        throw problem;
    } finally {
        clearTimeout(timer); signal?.removeEventListener('abort', abort);
    }
}

export function waitForMqttTest(signal, delay = 400) {
    return new Promise((resolve, reject) => {
        const abort = () => { clearTimeout(timer); reject(new DOMException('Aborted', 'AbortError')); };
        const timer = setTimeout(() => { signal.removeEventListener('abort', abort); resolve(); }, delay);
        if (signal.aborted) abort();
        else signal.addEventListener('abort', abort, { once: true });
    });
}
