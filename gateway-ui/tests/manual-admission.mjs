import assert from 'node:assert/strict';
import fs from 'node:fs';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { createServer } from 'vite';
import { NODE_COLUMNS } from '../src/nodeManagementColumns.js';
assert(!NODE_COLUMNS.some(([key]) => key === 'monitoring'));
const app = fs.readFileSync(new URL('../src/App.jsx', import.meta.url), 'utf8');
assert(!app.includes('className="monitoring-switch"'));
assert(!app.includes('新节点自动加入下方列表'));
assert(!app.includes('joinState.remaining} 秒'));
assert.match(app, /showPendingAdmissions = isScanning && joinIntent !== false/);
assert.match(app, /showPendingAdmissions && <>/);
globalThis.window = { location: { hostname: 'localhost' } };
const server = await createServer({ server: { middlewareMode: true }, appType: 'custom', ssr: { external: ['react', 'react-dom', 'lucide-react'] } });
try {
    const { default: Pending, PendingAdmissionTable } = await server.ssrLoadModule('/src/PendingAdmissionPanel.jsx');
    assert.match(renderToStaticMarkup(React.createElement(Pending, { onAuthorized() {} })), /待授权列表/);
    const table = renderToStaticMarkup(React.createElement(PendingAdmissionTable, {
        nodes: [{ mac: '01:02:03:04:05:06:07:08', name: 'HY-Sensor', model_name: 'HY-Sensor', lqi: 174, rssi: -42 }],
        refreshing: true
    }));
    assert.match(table, /刷新中…/);assert.match(table, /aria-busy="true"/);
    assert.match(table, /174/);assert.match(table, /-42/);
    const pendingSource = fs.readFileSync(new URL('../src/PendingAdmissionPanel.jsx', import.meta.url), 'utf8');
    assert.match(pendingSource, /pending\/refresh`.*, \{ method: 'POST' \}/);
    const { default: Home } = await server.ssrLoadModule('/src/HomeNodes.jsx');
    const html = renderToStaticMarkup(React.createElement(Home, {
        nodes: [{ mac: 'approved', name: '已授权设备' }], observed: {}, onOpen() {},
        activities: [{ mac: 'pending', name: '待授权设备', id: 1, time: Date.now(), text: '不能出现的活动' },
            { mac: 'approved', id: 2, time: Date.now(), text: '开始上传' }]
    }));
    assert.match(html, /开始上传/);assert.doesNotMatch(html, /不能出现的活动|待授权设备/);
    console.log('Manual admission UI PASS: pending panel, no monitoring switch/countdown, authorized-only home activities');
} finally { await server.close(); }
