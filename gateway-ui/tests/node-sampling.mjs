import assert from 'node:assert/strict';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { createServer } from 'vite';

const server = await createServer({ server: { middlewareMode: true }, appType: 'custom' });
try {
    const { NodeSamplingCard } = await server.ssrLoadModule('/src/App.jsx');
    const policy = {
        id: 'node:test', automatic_interval_minutes: 5, raw_upload_every_cycles: 1,
        vibration_enabled: true, vibration_window_seconds: 0.5,
        digital_microphone_enabled: true, digital_microphone_window_seconds: 1,
        analog_microphone_enabled: true, analog_microphone_window_seconds: 1
    };
    const render = (updates = {}, props = {}) => renderToStaticMarkup(React.createElement(NodeSamplingCard,
        { policy: { ...policy, ...updates }, onFieldChange() {}, onSave() {}, busy: false, ...props }));
    const html = render();
    assert.equal((html.match(/role="switch"/g) || []).length, 3);
    assert.equal((html.match(/type="checkbox"[^>]*checked/g) || []).length, 3);
    assert.equal((html.match(/<select/g) || []).length, 5);
    assert.equal((html.match(/readOnly/g) || []).length, 3);
    for (const label of ['振动采集', '数字麦克风采集', '超声麦克风采集', '保存配置'])
        assert.ok(html.includes(label));
    assert.match(html, /26.667 kHz/); assert.match(html, /46.875 kHz/); assert.match(html, /256 kHz/);
    assert.match(render({ vibration_enabled: false }), /is-vibration is-disabled/);
    assert.match(render({ vibration_enabled: false }), /<select[^>]*disabled=""[^>]*>/);
    assert.match(render({ automatic_interval_minutes: 7 }), /7分钟（当前配置）/);
    assert.match(render({ digital_microphone_window_seconds: 1.5 }), /1.5秒（旧配置）/);
    assert.match(render({}, { busy: true }), /<fieldset disabled=""[^>]*aria-busy="true"/);
    assert.match(render({}, { busy: true }), /保存中…/);
    assert.match(render({}, { notice: '保存失败' }), /node-sampling-status is-error/);
    assert.match(render({}, { notice: '保存成功' }), /node-sampling-status is-success/);
    assert.match(html, /role="status" aria-live="polite"/);
    console.log('Node sampling: switches, fixed rates, disabled controls, legacy options, busy and notices PASS');
} finally {
    await server.close();
}
