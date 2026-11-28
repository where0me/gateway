import assert from 'node:assert/strict';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { createServer } from 'vite';
import { handleProfileEditorKeyDown } from '../src/nodeProfileKeyboard.js';
const server = await createServer({ server: { middlewareMode: true }, appType: 'custom' });
try {
    const { default: Profile } = await server.ssrLoadModule('/src/NodeProfile.jsx');
    const key = (overrides = {}, busy = false) => {
        let prevented = false, submitted = false;
        handleProfileEditorKeyDown({ key: 'Enter', shiftKey: false, nativeEvent: {},
            preventDefault() { prevented = true; }, currentTarget: { form: { requestSubmit() { submitted = true; } } }, ...overrides }, busy);
        return { prevented, submitted };
    };
    assert.deepEqual(key(), { prevented: true, submitted: true });
    assert.deepEqual(key({ shiftKey: true }), { prevented: false, submitted: false });
    assert.deepEqual(key({ nativeEvent: { isComposing: true } }), { prevented: false, submitted: false });
    assert.deepEqual(key({ nativeEvent: { keyCode: 229 } }), { prevented: false, submitted: false });
    assert.deepEqual(key({}, true), { prevented: true, submitted: false });
    assert.deepEqual(key({ repeat: true }), { prevented: true, submitted: false });
    assert.deepEqual(key({ key: 'a' }), { prevented: false, submitted: false });
    const render = node => renderToStaticMarkup(React.createElement(Profile,
        { node, now: 20000, nodes: [node, { mac: 'another', name: '另一个节点' }], onSelect() {}, onRename() {}, onDescription() {}, onBack() {} }, React.createElement('input', { value: 300, readOnly: true })));
    const html = render({ mac: '0E:54:EF:00:25:E1:80:00', name: 'HY-Sensor-0E54', model_name: 'HY-Sensor', status: 'Online', short_addr: 0xBE44, link_updated_at: 10 });
    assert.equal((html.match(/role="tab"/g) || []).length, 2);
    assert.match(html, /node-tab-about/);
    assert.match(html, /node-tab-settings/);
    assert.match(html, /暂无节点图片/);
    assert.match(html, /0xBE44/);
    assert.match(html, /48708/);
    assert.match(html, /10秒前/);
    assert.match(html, /Online/);
    assert.match(html, /value="300"/);
    assert.match(html, /aria-label="切换节点"/);
    assert.match(html, /另一个节点/);
    assert.match(html, /title="重命名"/);
    assert.match(html, /title="自定义描述"/);
    assert.match(html, /<dt>描述<\/dt><dd class="node-profile-description"><div class="node-profile-description-display"><span>多传感器监测节点，支持温度、磁场、振动和声级采集。<\/span><button[^>]*title="自定义描述"/);
    assert.match(render({ mac: 'test', description: '<自定义描述>' }), /&lt;自定义描述&gt;/);
    const offline = render({ mac: 'test', name: 'Offline node', status: 'Offline' });
    assert.match(offline, /Offline/);
    assert.match(offline, /未上报/);
    assert.doesNotMatch(offline, /0x0000/);
    assert.match(render({ mac: 'test', short_addr: 0, image_url: '/node-photo.png' }), /0x0000/);
    console.log('Node profile: two tabs, saved form, real/unknown addresses, last seen and offline state PASS');
} finally { await server.close(); }
