import assert from 'node:assert/strict';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { createServer } from 'vite';
import { normalizeTrendRecords, TREND_METRICS, trendBounds } from '../src/sensorTrend.js';
import { nodeCardWidth } from '../src/nodeDetailsLayout.js';

assert.equal(nodeCardWidth(1048), 340);
assert.equal(nodeCardWidth(694), 340);
assert.equal(nodeCardWidth(300), 300);
assert.equal(nodeCardWidth(0), 340);
assert.equal(nodeCardWidth(1232), (1232-28)/3);

const samples = Array.from({ length: 40 }, (_, index) => ({ batch_id: index+1, timestamp: 1000+index,
    temperature: index, digital_db: index, analog_db: index, mag_x: -index, mag_y: 0, mag_z: index }));
for (const metric of Object.keys(TREND_METRICS)) {
    const rows = normalizeTrendRecords([...samples].reverse(), metric);
    assert.equal(rows.length, 30); assert.equal(rows[0].batch_id, 11); assert.equal(rows.at(-1).batch_id, 40);
}
assert.equal(normalizeTrendRecords([samples[0], samples[0]], 'temperature').length, 1);
assert.equal(normalizeTrendRecords([{ ...samples[0], temperature: null }], 'temperature').length, 0);
assert.equal(normalizeTrendRecords([{ ...samples[0], mag_y: null }], 'magnetic').length, 0);
assert.equal(normalizeTrendRecords([samples[0]], 'temperature')[0].temperature, 0);
assert.equal(normalizeTrendRecords([null, {}], 'temperature').length, 0);
assert.equal(normalizeTrendRecords(samples, 'invalid').length, 0);
const bounds = trendBounds([samples[0]], TREND_METRICS.temperature.series);
assert.ok(bounds[0] < 0 && bounds[1] > 0);

const captureOrdered = normalizeTrendRecords([
    { ...samples[0], batch_id: 1, timestamp: 9000, captured_at: 1000 },
    { ...samples[0], batch_id: 2, timestamp: 8000, captured_at: 2000 }
], 'temperature');
assert.deepEqual(captureOrdered.map(row => row.batch_id), [1, 2]);
const server = await createServer({ server: { middlewareMode: true }, appType: 'custom' });
try {
    const { SensorTrendChart, default: Panel } = await server.ssrLoadModule('/src/SensorTrendPanel.jsx');
    const { default: Card } = await server.ssrLoadModule('/src/NodeDetails.jsx');
    const { default: List } = await server.ssrLoadModule('/src/NodeDetailsList.jsx');
    const nodes = [{ mac: 'one', name: 'node one' }, { mac: 'two', name: 'node two' }];
    const list = renderToStaticMarkup(React.createElement(List, { devices: nodes,
        selectedTrend: { mac: 'two', metric: 'temperature' }, onCloseTrend() {},
        renderNode: dev => React.createElement('article', { className: 'compact-node' }, dev.name) }));
    assert.equal((list.match(/node-details-item has-trend/g) || []).length, 1);
    assert.match(list, /node-details-item has-trend"><article class="compact-node">node two<\/article><aside/);
    const records = normalizeTrendRecords(samples, 'magnetic');
    const chart = renderToStaticMarkup(React.createElement(SensorTrendChart, { records, metric: 'magnetic' }));
    assert.equal((chart.match(/<polyline/g) || []).length, 3);
    assert.equal((chart.match(/<circle/g) || []).length, 90);
    assert.match(chart, /共 30 次采集/); assert.match(chart, /μT/); assert.match(chart, /批次 40/);
    const single = renderToStaticMarkup(React.createElement(SensorTrendChart, { records: [samples[0]], metric: 'temperature' }));
    assert.ok(!single.includes('NaN') && !single.includes('Infinity')); assert.match(single, /0.00/);
    const panel = renderToStaticMarkup(React.createElement(Panel, { mac: 'test', name: 'HY-Sensor', metric: 'temperature', onClose() {} }));
    assert.match(panel, /最近 0 \/ 30 次采集/); assert.match(panel, /关闭趋势图/);
    const card = renderToStaticMarkup(React.createElement(Card, { dev: { mac: 'test', name: 'HY-Sensor' },
        index: 0, parseMagData: () => null, onTrend() {}, activeTrend: 'temperature' }));
    assert.equal((card.match(/compact-trend-button/g) || []).length, 4);
    assert.match(card, /aria-pressed="true"/); assert.match(card, /查看磁场趋势/);
    const timedChart = renderToStaticMarkup(React.createElement(SensorTrendChart, {
        records: captureOrdered, metric: 'temperature' }));
    assert.ok(timedChart.includes(new Date(2000 * 1000).toISOString()));
    assert.ok(!timedChart.includes(new Date(8000 * 1000).toISOString()));
    assert.doesNotMatch(timedChart, /上报时间/);
    assert.match(single, /采集时间未记录/);
    console.log('Sensor trends: 30-batch cap, ordering/deduplication, null/zero data, single point, 3-axis chart and four buttons PASS');
} finally { await server.close(); }
