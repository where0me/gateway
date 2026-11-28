import assert from 'node:assert/strict';
import { createServer } from 'vite';
const server = await createServer({ server: { middlewareMode: true }, appType: 'custom' });
try {
    const { activityText } = await server.ssrLoadModule('/src/homeActivity.js');
    const previous = { lqi: 149, digital_db: '67.00', analog_db: '69.00' };
    const status = (state, lqi = 160) => ({ type: 'monitor_update', collection_state: state, lqi, batch_id: 42 });
    assert.equal(activityText(status('idle'), previous), 'LQI：149 → 160');
    assert.equal(activityText(status('idle', 149), previous), null);
    assert.equal(activityText(status('idle', 0), previous), 'LQI：149 → 0');
    assert.equal(activityText(status('idle', 0), {}), 'LQI：0');
    assert.equal(activityText(status('idle', -1), previous), null);
    assert.equal(activityText({ type: 'monitor_update', collection_state: 'idle' }, previous), null);
    for (const state of ['waiting', 'capturing', 'unknown']) assert.equal(activityText(status(state), previous), null);
    assert.equal(activityText(status('uploading'), previous), '开始上传 · 批次 42');
    assert.equal(activityText(status('complete'), previous), '上传完成 · 批次 42');
    assert.equal(activityText(status('error'), previous), '采集或上传失败 · 批次 42');
    assert.equal(activityText({ type: 'sound_update', source: 'digital', db: '70.00' }, previous), '数字麦克风声级：67.00 → 70.00 dB SPL');
    assert.equal(activityText({ type: 'sound_update', source: 'analog', db: '71.00' }, previous), '超声麦克风声级：69.00 → 71.00 dB SPL');
    for (const raw_complete of [false, true]) assert.equal(activityText({ type: 'collection_data_ready', batch_id: 42, raw_complete }), null);
    for (const state of ['queued', 'uploading', 'complete']) assert.equal(activityText({ type: 'vibration_upload_progress', status: state, batch_id: 42 }), null);
    assert.equal(activityText({ type: 'status_update', status: 'Online' }), '网络状态：Online');
    assert.equal(activityText({ type: 'temp_update', temp: '24.50' }), '温度：24.50 ℃');
    assert.equal(activityText({ type: 'name_update', name: '锅炉' }), '节点名称已更新：锅炉');
    assert.equal(activityText({ type: 'monitor_setting', enabled: true }), null);
    console.log('Activity: idle LQI changes only, collection suppression, completion after final state, microphone names PASS');
} finally { await server.close(); }
