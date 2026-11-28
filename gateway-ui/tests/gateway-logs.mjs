import assert from 'node:assert/strict';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { createServer } from 'vite';
import { logsCsv, logParameters, readLogClearBoundary, saveLogClearBoundary } from '../src/gatewayLogsFormat.js';
const stored = new Map();
const storage = { getItem: key => stored.get(key) ?? null, setItem: (key, value) => stored.set(key, value) };
assert.equal(readLogClearBoundary(storage, 'gateway'), 0);
saveLogClearBoundary(36, storage, 'gateway');
// Re-entering the page reads the same boundary; only subsequent records remain visible.
const restoredBoundary = readLogClearBoundary(storage, 'gateway');
assert.equal(restoredBoundary, 36);
assert.deepEqual([{ id: 35 }, { id: 36 }, { id: 37 }].filter(row => row.id > restoredBoundary), [{ id: 37 }]);
assert.equal(readLogClearBoundary(storage, 'other-gateway'), 0);
assert.equal(readLogClearBoundary({ getItem: () => 'invalid' }, 'gateway'), 0);
assert.equal(readLogClearBoundary({ getItem: () => { throw new Error('disabled'); } }, 'gateway'), 0);
assert.doesNotThrow(() => saveLogClearBoundary(36, { setItem: () => { throw new Error('disabled'); } }, 'gateway'));
assert.equal(readLogClearBoundary(), 0); // Server-side rendering has no browser storage.
const csv = logsCsv([{ timestamp: 1704067200, level: 'ERROR', source: 'node', mac: 'test', short_addr: 0x1234, batch: 4294967295, event: 'failure', message: '=bad,"quoted"\nnext' }]);
assert.match(csv, /0x1234/); assert.match(csv, /4294967295/);
assert.match(csv, /'=bad,""quoted""/); assert.equal(csv[0], '\uFEFF');
const server = await createServer({ server: { middlewareMode: true }, appType: 'custom' });
try {
    const { default: Logs, LogsConsole } = await server.ssrLoadModule('/src/GatewayLogs.jsx');
    const html = renderToStaticMarkup(React.createElement(Logs, { nodes: [{ mac: 'test', name: '测试节点' }] }));
    for (const text of ['日志', '查询日志', '导出日志', '实时日志筛选', '日志上限', '清空', '刷新', '测试节点', 'logs-console', '日志窗口']) assert.ok(html.includes(text));
    assert.ok(!html.includes('每 2 秒自动更新'));
    const buttons = [...html.matchAll(/<button\b[^>]*>([\s\S]*?)<\/button>/g)].map(match => match[1].replace(/<[^>]*>/g, ''));
    assert.deepEqual(buttons, ['清空', '查询日志', '导出日志', '刷新']);
    assert.ok(!html.includes('datetime-local'));
    assert.ok(!html.includes('导出本页'));
    assert.equal(logParameters({ level: 'ERROR', source: 'node', mac: 'AA:BB', from: '', to: '' }).get('level'), 'ERROR');
    assert.equal(logParameters({}).get('limit'), '100');
    for (const limit of [100, 200, 500, 1000]) assert.equal(logParameters({ limit }).get('limit'), String(limit));
    assert.throws(() => logParameters({ from: '2026-10-05T15:00', to: '2026-10-05T14:00' }), /结束时间/);
    assert.throws(() => logParameters({ from: 'invalid' }), /时间格式/);
    const consoleHtml = renderToStaticMarkup(React.createElement(LogsConsole, { nodes: [], data: { items: [{ id: 1, timestamp: 1704067200, level: 'ERROR', source: 'node', event: 'failure', message: '<script>bad</script>', mac: 'AA:BB', short_addr: 0x1234, batch: 9 }] } }));
    assert.ok(consoleHtml.includes('log-error')); assert.ok(consoleHtml.includes('0x1234')); assert.ok(consoleHtml.includes('&lt;script&gt;')); assert.ok(!consoleHtml.includes('<script>'));
    const warningHtml = renderToStaticMarkup(React.createElement(LogsConsole, { nodes: [], data: { items: [{ id: 2, timestamp: 1704067200, level: 'WARN', source: 'node', event: 'sensor_capture_missing', message: '本批采集时已启用但未采到：温度、数字麦克风声级；继续上传本批其余数据', mac: 'AA:BB', short_addr: 0x1234, batch: 9 }] } }));
    for (const text of ['log-warn', '采集缺项', '数字麦克风声级', '批次=9', '继续上传']) assert.ok(warningHtml.includes(text));
    for (const [event, label] of [['control_retry', '控制命令重发'], ['upload_reservation', '上传名额'], ['monitor_applied', '配置已生效']]) {
        const text = renderToStaticMarkup(React.createElement(LogsConsole, { nodes: [], data: { items: [{ id: 3, timestamp: 1704067200, level: 'INFO', source: 'backend', event, message: '状态说明', mac: 'AA:BB', batch: 9 }] } }));
        assert.ok(text.includes(label));
    }
    for (const text of ['上一页', '下一页', '实时更新', 'type="checkbox"']) assert.ok(!html.includes(text));
    assert.ok(!html.includes('关键字')); assert.ok(!html.includes('name="q"')); assert.ok(!html.includes('<table'));
    console.log('Gateway logs page and safe CSV export PASS');
} finally { await server.close(); }
