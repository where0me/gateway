import assert from 'node:assert/strict';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { createServer } from 'vite';
const server = await createServer({ server: { middlewareMode: true }, appType: 'custom' });
try {
    const { default: Card } = await server.ssrLoadModule('/src/NodeDetails.jsx');
    const render = dev => renderToStaticMarkup(React.createElement(Card, {
        dev, index: 0, parseMagData: () => null, onRemove: () => {}
    }));
    const html = render({ name: 'HY-Sensor', status: 'Online', lqi: 149, rssi: -46, power_source: 4 });
    assert.match(html, /LQI/);
    assert.match(html, />149</);
    assert.match(html, /-46 dBm/);
    assert.match(html, /外部供电/);
    assert.match(html, /剩余电量：不适用/);
    assert.match(html, /compact-device-metrics/);
    assert.match(html, /disabled/); // No raw file until upload is complete.
    const unknown = render({ status: 'Online', lqi: 0, rssi: -128 });
    assert.match(unknown, /LQI 0/);
    assert.ok(!unknown.includes('-128 dBm'));
    assert.match(unknown, /剩余电量：未上报/);
    const battery = render({ status: 'Online', power_source: 3, battery_percentage: 0 });
    assert.match(battery, /剩余电量：0%/);
    const offline = render({ status: 'Offline', lqi: 149, rssi: -46 });
    assert.match(offline, /LQI：节点离线/);
    assert.ok(!offline.includes('-46 dBm'));
    assert.match(render({ status: 'Online', raw_upload_status: 'complete', vibration_features_ready: true }), /原始波形上传完成/);
    const uploading = render({ status: 'Online', raw_upload_status: 'uploading', upload_started: true,
        raw_received_packets: 12, raw_total_packets: 27, raw_progress_unit: 'blocks' });
    assert.match(uploading, /12\/27 窗口（44%）/);
    assert.match(uploading, /<progress[^>]*max="27"[^>]*value="12"/);
    const completed = render({ status: 'Online', raw_upload_status: 'complete',
        vibration_features_ready: true, vibration_features: [[0.123, 1, 2, 3, 4], [], []] });
    assert.match(completed, /0.123/);
    assert.doesNotMatch(completed, /disabled=""/);
    const retained = render({ status: 'Online', temp: '24.50', digital_db: '0.00', analog_db: '61.00',
        collection_id: 3, collection_state: 'capturing', raw_upload_status: 'waiting',
        vibration_features_ready: false, vibration_features: [[0.123, 1, 2, 3, 4], [], []],
        vibration_features_timestamp: 100, last_reading_time: '2026/10/26 13:14:30',
        last_complete: { collection_id: 2, temp: '24.50', digital_db: '0.00', analog_db: '61.00',
            raw_upload_status: 'complete', vibration_features_ready: true,
            vibration_features: [[0.123, 1, 2, 3, 4], [], []], last_reading_time: '2026/10/26 13:14:30' } });
    assert.doesNotMatch(retained, /24.50 ℃|0.00 dB SPL|0.123|2026\/10\/26 13:14:30/);
    assert.match(retained, /正在采集传感器数据/);
    assert.match(retained, /本批采集时间未记录/);
    assert.match(retained, /本次振动特征/);
    assert.equal((retained.match(/disabled=""/g) || []).length, 2);
    const queued = render({ collection_id: 3, cycle_active: true, upload_started: false,
        raw_upload_status: 'queued', collection_state: 'waiting', last_complete: { ...retained } });
    assert.match(queued, /等待数据上传/);
    assert.equal((queued.match(/disabled=""/g) || []).length, 2);
    const rawOnly = render({ upload_started: true, raw_upload_status: 'complete', vibration_features_ready: false });
    assert.match(rawOnly, /等待振动特征/);
    assert.equal((rawOnly.match(/disabled=""/g) || []).length, 2);
    const captured = render({ status: 'Online', capture_time: '2026/11/3 20:30:00',
        last_reading_time: '2026/11/3 20:32:10', last_sample_time: '2026/11/3 20:32:10' });
    assert.match(captured, /采集时间：2026\/11\/3 20:30:00/);
    assert.doesNotMatch(captured, /20:32:10/);
    assert.match(render({ last_reading_time: 'upload-only' }), /采集时间未记录/);
    assert.doesNotMatch(render({ last_reading_time: 'upload-only' }), /upload-only/);
    console.log('Compact node card: RSSI/LQI, zero values, truthful power/battery, offline and waveform states PASS');
} finally { await server.close(); }
