import assert from 'node:assert/strict';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { createServer } from 'vite';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { mqttDraftFromConfig, validateMqttSettings, mqttSettingsPayload, formatMqttBytes, mqttRequest, waitForMqttTest } from '../src/mqttSettings.js';

const configuration = { enabled: false, url: 'mqtt://broker.internal:1883', gateway_id: 'real-gateway', client_id: '',
    username: 'gateway', password: 'saved-test-secret', password_set: true, keepalive_sec: 60, ack_timeout_ms: 15000, connect_timeout_ms: 1000 };
const initial = mqttDraftFromConfig(configuration);
assert.equal(initial.password, 'saved-test-secret');
assert.equal(mqttDraftFromConfig({ ...configuration, password_from_env: true }).password, '');
assert.equal(mqttDraftFromConfig({ ...configuration, password: '', password_set: false }).password, '');
assert.equal(initial.host, 'broker.internal');
assert.equal(Object.hasOwn(initial, 'protocol'), false);
assert.equal(initial.groupId, 'factory01');
assert.equal(mqttDraftFromConfig({ ...configuration, protocol: 'json' }).groupId, 'factory01');
const sparkplug = { ...initial, groupId: 'line01' };
assert.deepEqual(validateMqttSettings(sparkplug), {});
assert.ok(validateMqttSettings({ ...sparkplug, groupId: 'bad/group' }).groupId);
assert.ok(validateMqttSettings({ ...sparkplug, groupId: '' }).groupId);
assert.equal(mqttSettingsPayload(sparkplug, configuration).protocol, 'sparkplug_b');
assert.equal(mqttSettingsPayload({ ...sparkplug, protocol: 'json' }, configuration).protocol, 'sparkplug_b');
assert.equal(mqttSettingsPayload(sparkplug, configuration).group_id, 'line01');
assert.deepEqual(validateMqttSettings(initial), {});
for (const [key, value] of [['host', 'mqtt://server:1883'], ['host', 'server:1883'], ['host', ''], ['host', 'a b'],
    ['port', '0'], ['port', '65536'], ['port', '1883.5'], ['gatewayId', 'bad/id'], ['gatewayId', ''],
    ['clientId', 'bad/id'], ['keepalive', '0'], ['ackTimeout', '301'], ['username', 'u'.repeat(128)], ['password', 'p'.repeat(256)]]) {
    assert.ok(validateMqttSettings({ ...initial, enabled: true, [key]: value })[key], `${key}=${value} must be rejected`);
}
for (const [key, value] of [['host', 'broker.company.local'], ['host', '[::1]'], ['port', '65535'],
    ['gatewayId', 'gw-002'], ['username', ''], ['password', '']]) {
    assert.deepEqual(validateMqttSettings({ ...initial, [key]: value }), {});
}
assert.deepEqual(validateMqttSettings({ ...initial, host: '', enabled: false }), {});
assert.ok(validateMqttSettings({ ...initial, host: '', enabled: true }).host);
for (const key of ['collectionTopic', 'waveformTopic', 'topicPrefix', 'clear_password']) assert.equal(Object.hasOwn(mqttSettingsPayload(initial, configuration), key), false);
assert.equal(mqttSettingsPayload(initial, configuration).password, 'saved-test-secret');
assert.equal(mqttSettingsPayload({ ...initial, password: '' }, configuration).clear_password, true);
assert.equal(Object.hasOwn(mqttSettingsPayload({ ...initial, password: '' }, { ...configuration, password_set: false }), 'clear_password'), false);
assert.equal(mqttSettingsPayload({ ...initial, password: 'new-secret' }, configuration).password, 'new-secret');
const managed = mqttSettingsPayload({ ...initial, password: 'ignored' }, { ...configuration, password_from_env: true, username_from_env: true });
assert.equal(Object.hasOwn(managed, 'password'), false);assert.equal(Object.hasOwn(managed, 'clear_password'), false);assert.equal(Object.hasOwn(managed, 'username'), false);
assert.deepEqual(formatMqttBytes(0), { value: '0', unit: 'B' });
assert.deepEqual(formatMqttBytes(2048), { value: '2.0', unit: 'KB' });
assert.deepEqual(formatMqttBytes(null), { value: '—', unit: '' });

// Run the actual component controller with deterministic hooks and an API seam.
// Unlike the removed screen demo, this exists only in the test runner.
const source = await readFile(new URL('../src/MqttSettings.jsx', import.meta.url), 'utf8');
const controllerSource = source.slice(source.indexOf('export default function MqttSettings() {') + 'export default function MqttSettings() {'.length, source.indexOf('    const field ='));
assert.ok(controllerSource.includes('async function submit'));
function harness() {
    let cursor = 0, slots = [], effects = [], timers = new Map(), nextTimer = 0;
    let serverConfig = { ...configuration }, serverRuntime = { state: 'disabled', pending_batches: 17, pending_bytes: 32768, last_confirmed_at: null, queue_available: true };
    let failSave = false, delayedStatus = null;
    const calls = [];
    const useState = initialValue => {
        const index = cursor++;
        if (!slots[index]) slots[index] = { value: typeof initialValue === 'function' ? initialValue() : initialValue };
        return [slots[index].value, value => { slots[index].value = typeof value === 'function' ? value(slots[index].value) : value; }];
    };
    const useRef = value => { const index = cursor++; return slots[index] ||= { current: value }; };
    const useEffect = (fn, deps) => {
        const index = cursor++, previous = slots[index];
        if (!previous || deps.some((value, i) => value !== previous.deps[i])) {
            previous?.cleanup?.(); const effect = { deps, cleanup: null }; slots[index] = effect;
            effects.push(() => { effect.cleanup = fn(); });
        }
    };
    const request = async (path, options = {}) => {
        calls.push({ path, ...options });
        if (options.signal?.aborted) throw new DOMException('Aborted', 'AbortError');
        if (path === 'test') return { id: 'actual-test-id', state: 'testing' };
        if (path.startsWith('test?id=')) return { state: 'success', message: '连接和账号认证成功；不验证发布权限' };
        if (path === 'status' && delayedStatus) return delayedStatus;
        if (options.body) {
            if (failSave) throw new Error('config write failure');
            serverConfig = { ...serverConfig, ...options.body, password_set: options.body.clear_password ? false : Boolean(options.body.password) || serverConfig.password_set };
            if (options.body.clear_password) serverConfig.password = '';
            delete serverConfig.clear_password; serverRuntime = { ...serverRuntime, state: serverConfig.enabled ? 'connecting' : 'disabled' };
        }
        return { config: serverConfig, runtime: { ...serverRuntime }, config_valid: true };
    };
    const context = { useState, useRef, useEffect, mqttDraftFromConfig, validateMqttSettings, mqttSettingsPayload, MQTT_STATE_LABELS: { disabled: '未启用', connected: '已连接', connecting: '连接中' },
        mqttRequest: request, waitForMqttTest: async signal => { if (signal.aborted) throw new DOMException('Aborted', 'AbortError'); },
        AbortController, Date, encodeURIComponent, window: {
            setInterval(fn, interval) { assert.equal(interval, 3000); timers.set(++nextTimer, fn); return nextTimer; },
            clearInterval(id) { timers.delete(id); }
        } };
    return {
        calls, render() { cursor = 0; const result = vm.runInNewContext(`(function(){${controllerSource}\nreturn {draft,saved,config,runtime,dirty,status,statusError,loadError,notice,operationError,busy,update,submit};})()`, context); effects.splice(0).forEach(fn => fn()); return result; },
        async flush() { for (let i = 0; i < 6; i++) await Promise.resolve(); },
        async poll() { for (const fn of timers.values()) await fn(); },
        failSave(value) { failSave = value; },
        delayStatus() { return new Promise(resolve => { delayedStatus = new Promise(done => { resolve(done); }); }); },
        cleanup() { slots.forEach(slot => slot?.cleanup?.()); }, timers
    };
}
const model = harness();assert.equal(model.render().draft, null);await model.flush();
let view = model.render();assert.equal(view.draft.host, 'broker.internal');assert.equal(view.runtime.pending_batches, 17);
assert.equal(view.draft.password, 'saved-test-secret');
view.update('host', 'changed.internal');await model.poll();view = model.render();assert.equal(view.draft.host, 'changed.internal');
await view.submit(true);view = model.render();
assert.equal(view.notice, '');assert.equal(view.runtime.state, 'disabled');
assert.equal(model.calls.filter(call => call.path === 'settings' && call.body).length, 0);
view.update('enabled', true);view = model.render();await view.submit(false);view = model.render();
assert.equal(view.runtime.state, 'connecting');assert.equal(view.dirty, false);assert.equal(view.draft.password, 'saved-test-secret');
assert.equal(model.calls.filter(call => call.path === 'settings' && call.body).length, 1);
view.update('host', 'unsaved.internal');view = model.render();model.failSave(true);await view.submit(false);view = model.render();
assert.equal(view.draft.host, 'unsaved.internal');assert.equal(view.saved.host, 'changed.internal');assert.match(view.operationError, /write failure/);
model.failSave(false);
view.update('password', 'replacement-test-secret');view = model.render();await view.submit(false);view = model.render();
assert.equal(view.draft.password, 'replacement-test-secret');assert.equal(view.saved.password, 'replacement-test-secret');
await model.poll();view = model.render();assert.equal(view.draft.password, 'replacement-test-secret');
view.update('password', '');view = model.render();await view.submit(true);view = model.render();
assert.equal(model.calls.findLast(call => call.path === 'test').body.clear_password, true);
assert.equal(view.saved.password, 'replacement-test-secret');
await view.submit(false);view = model.render();
assert.equal(view.draft.password, '');assert.equal(view.config.password_set, false);assert.equal(view.dirty, false);
const releaseStatus = await model.delayStatus();const poll = model.poll();
view.update('enabled', false);view = model.render();await view.submit(false);
releaseStatus({ runtime: { state: 'connected', pending_batches: 999 }, config_valid: true });await poll;
view = model.render();assert.equal(view.runtime.state, 'disabled');assert.equal(view.runtime.pending_batches, 17);
model.cleanup();assert.equal(model.timers.size, 0);
const strict = harness();strict.render();strict.cleanup();await strict.flush();assert.equal(strict.render().draft, null);strict.cleanup();

const previousWindow = globalThis.window, previousFetch = globalThis.fetch;
try {
    globalThis.window = { location: { hostname: 'test-host' } };
    globalThis.fetch = async (url, options) => {
        assert.equal(url, 'http://test-host:8080/api/mqtt/settings');
        assert.equal(options.method, 'POST');assert.equal(options.cache, 'no-store');
        assert.equal(JSON.parse(options.body).password, 'only-in-request-body');
        return { ok: true, async json() { return { status: 'success' }; } };
    };
    await mqttRequest('settings', { body: { password: 'only-in-request-body' } });
    globalThis.fetch = async () => ({ ok: false, async json() { return { status: 'error', msg: 'save failed' }; } });
    await assert.rejects(mqttRequest('settings'), /save failed/);
    const abort = new AbortController();abort.abort();await assert.rejects(waitForMqttTest(abort.signal), { name: 'AbortError' });
} finally { globalThis.window = previousWindow; globalThis.fetch = previousFetch; }
const server = await createServer({ server: { middlewareMode: true }, appType: 'custom' });
try {
    const { default: SettingsPage } = await server.ssrLoadModule('/src/SettingsPage.jsx');
    const { NAVIGATION_ITEMS } = await server.ssrLoadModule('/src/gatewayNavigation.js');
    assert.equal(NAVIGATION_ITEMS.find(([key]) => key === 'settings')[1], '设置');
    const html = renderToStaticMarkup(React.createElement(SettingsPage, null, React.createElement('p', null, '原有信道设置')));
    for (const text of ['MQTT 上传', 'Zigbee 网络', '正在读取 MQTT 配置', '原有信道设置']) assert.ok(html.includes(text), text);
    assert.match(html, /id="settings-tab-mqtt" role="tab" aria-selected="true"/);
    assert.match(html, /id="settings-panel-zigbee"[^>]*hidden=""/);
    assert.ok(source.indexOf('className="mqtt-panel mqtt-upload-status"') < source.indexOf('className="mqtt-panel mqtt-connection"'),
        'Upload status must appear above the connection configuration');
    assert.doesNotMatch(html, /<textarea |自定义发布主题|采集结果发布主题|原始波形发布主题|占位符|示例主题|mqtt-topic/);
    assert.doesNotMatch(html, /name="collectionTopic"|name="waveformTopic"/);
    assert.doesNotMatch(html, /MQTTX 订阅主题|复制订阅主题|查看发布主题|主题前缀|name="topicPrefix"/);
    assert.doesNotMatch(html, /mqtts:\/\/|1234/);
    assert.doesNotMatch(source, /FlaskConical|模拟|演示|示例时间|16:15:35|234\.4|scenario|localStorage|sessionStorage/);
    assert.doesNotMatch(source, /clearPassword|保存时清除现有密码|留空保持不变|mqtt-clear-password/);
    assert.doesNotMatch(source, /测试使用独立客户端|变更服务器、账号或网关 ID 将作为新的上传目标|服务器确认接收不代表历史数据已入库|mqtt-delivery-note/);
    assert.match(source, /type: showPassword \? 'text' : 'password'/);
    assert.match(source, /controller\.abort\(\)/);
    assert.match(source, /aria-live="polite"/);
    assert.doesNotMatch(source, /topicField|topicErrors|validateMqttTopicTemplates|renderMqttTopic|MQTT_TOPIC/);
    assert.doesNotMatch(source, /name="protocol"|value="json"|JSON 批次|broker_puback|最近上传确认/);
    assert.match(source, /Sparkplug B \/ MQTT 3\.1\.1/);
    assert.match(source, /<span>待上传批次总数<\/span>/);
    assert.doesNotMatch(source, /待上传统计为当前目标已入队的批次|关闭期间的本地采集记录会在重新启用后继续整理和补发/);
    assert.match(source, /field\('groupId'/);
    const css = await readFile(new URL('../src/SettingsPage.css', import.meta.url), 'utf8');
    assert.doesNotMatch(css, /\.mqtt-topic/);
    console.log('MQTT settings: real API payloads/passwords, test vs production state, polling draft/stale protection, failed saves, cleanup, no screen demo/custom topics PASS');
} finally { await server.close(); }
