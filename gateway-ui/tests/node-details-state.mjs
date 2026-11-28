import assert from 'node:assert/strict';
import { mergeDeviceSnapshot, mergeLiveCollection, mergeVibrationDetails, nodeCardView, uploadStatusText } from '../src/nodeDetailsState.js';
const format = time => `time:${time}`;
const features = [[1, 2, 3, 4, 5], [0, 0, 0, 0, 0], [6, 7, 8, 9, 10]];
const saved = { batch_id: 100, timestamp: 1000, sample_rate_hz: 26667, sample_count: 13334,
    raw_available: true, features_ready: true, features, upload_started: true,
    collection_values: { temperature: 24.5, digital_db: 60, analog_db: 62, magnetic: { x: 1, y: 2, z: 3 } } };
const previous = mergeVibrationDetails({ mac: 'one', status: 'Online' }, saved, format);
assert.equal(nodeCardView(previous).waveform_available, true);
let node = mergeLiveCollection(previous, { type: 'monitor_update', batch_id: 3, collection_state: 'capturing' }, format);
let view = nodeCardView(node);
assert.equal(node.collection_id, 3); // IDs can decrease after a reboot.
for (const key of ['temp', 'digital_db', 'analog_db', 'mag']) assert.equal(view[key], '--');
assert.deepEqual(view.vibration_features, []);
assert.equal(view.last_reading_time, '');assert.equal(view.waveform_available, false);
assert.equal(uploadStatusText(node), '正在采集传感器数据');
assert.equal(mergeVibrationDetails(node, saved, format), node); // Old batch cannot refill the card.
node = mergeLiveCollection(node, { type: 'vibration_upload_progress', batch_id: 3, status: 'queued', received_packets: 0, total_packets: 27 });
assert.equal(uploadStatusText(node), '等待数据上传');assert.equal(nodeCardView(node).temp, '--');
const queuedNode = node;
assert.equal(mergeLiveCollection(node, { type: 'monitor_update', batch_id: 3, collection_state: 'capturing' }), queuedNode);
node = mergeDeviceSnapshot(node, { mac: 'one', collection_state: 'capturing', collection_batch_id: 3, capture_batch_id: 3 }, format);
assert.equal(node.collection_state, 'waiting');assert.equal(uploadStatusText(node), '等待数据上传');
assert.equal(uploadStatusText({ ...node, collection_state: 'capturing' }), '等待数据上传');
assert.equal(nodeCardView(node).waveform_available, false);
node = mergeLiveCollection(node, { type: 'collection_data_ready', batch_id: 3, raw_complete: false });
assert.equal(nodeCardView(node).waveform_available, false);
const descriptor = { ...saved, batch_id: 3, timestamp: 1100, upload_started: false, features_ready: false,
    features: [], raw_available: false, raw_received_packets: 0, raw_total_packets: 27, collection_values: {}, last_complete: saved };
node = mergeVibrationDetails(node, descriptor, format);
assert.equal(nodeCardView(node).temp, '--');assert.equal(nodeCardView(node).last_sample_time, '');
// First real sensor packet switches to this batch immediately, including zero.
node = mergeLiveCollection(node, { type: 'sound_update', batch_id: 3, source: 'digital', db: 0, timestamp: 1101 }, format);
view = nodeCardView(node);assert.equal(view.digital_db, '0.00');assert.equal(view.temp, '--');
assert.equal(view.last_reading_time, 'time:1101');assert.equal(view.waveform_available, false);
node = mergeDeviceSnapshot(node, { mac: 'one', collection_state: 'capturing', collection_batch_id: 3, capture_batch_id: 3,
    temp: '99', latest_readings: { temperature: 99, digital_db: 60, features, features_batch_id: 100, timestamp: 1000 } }, format);
assert.equal(nodeCardView(node).digital_db, '0.00');assert.equal(nodeCardView(node).temp, '--');
assert.equal(node.collection_state, 'uploading');
node = mergeVibrationDetails(node, { ...descriptor, upload_started: true, collection_values: { temperature: 31, digital_db: 68 },
    raw_received_packets: 1, raw_progress_unit: 'blocks' }, format);
assert.equal(nodeCardView(node).temp, '31.00');assert.equal(nodeCardView(node).digital_db, '0.00');
assert.equal(nodeCardView(node).analog_db, '--');assert.deepEqual(nodeCardView(node).vibration_features, []);
assert.match(uploadStatusText(node), /1\/27 窗口/);
node = mergeLiveCollection(node, { type: 'temp_update', batch_id: 3, temp: 0, timestamp: 1102 }, format);
node = mergeVibrationDetails(node, { ...descriptor, upload_started: true, collection_values: { temperature: null } }, format);
assert.equal(nodeCardView(node).temp, '0.00');
const staleComplete = mergeLiveCollection(node, { type: 'monitor_update', batch_id: 100, collection_state: 'complete' });
assert.equal(staleComplete, node);
// A later acquisition does not destroy an older batch currently on the wire.
const overlapping = mergeLiveCollection(node, { type: 'monitor_update', batch_id: 4, collection_state: 'capturing' });
assert.equal(overlapping.collection_id, 3);assert.equal(overlapping.pending_collection_id, 4);
node = mergeVibrationDetails(node, { ...descriptor, raw_available: true, upload_started: true }, format);
assert.equal(nodeCardView(node).waveform_available, false);
assert.equal(uploadStatusText(node), '原始波形已接收，等待振动特征');
node = mergeVibrationDetails(node, { ...saved, batch_id: 3 }, format);
assert.equal(nodeCardView(node).waveform_available, true);assert.deepEqual(nodeCardView(node).vibration_features, features);
assert.equal(mergeLiveCollection(node, { type: 'vibration_upload_progress', batch_id: 3, status: 'queued' }), node);
node = mergeVibrationDetails(node, descriptor, format);
assert.equal(node.raw_upload_status, 'complete');assert.equal(node.vibration_features_ready, true);
const drained = mergeVibrationDetails(overlapping, { ...saved, batch_id: 3 }, format);
assert.equal(nodeCardView(drained).collection_id, 4);assert.equal(nodeCardView(drained).temp, '--');
assert.equal(nodeCardView(drained).waveform_available, false);assert.equal(uploadStatusText(drained), '等待数据上传');
// Cold reload while capturing/queued must use the persisted current batch ID.
const latest = { temperature: 24.5, features, features_batch_id: 100, timestamp: 1000 };
for (const state of ['capturing', 'waiting']) {
    let cold = mergeDeviceSnapshot(undefined, { mac: 'one', collection_state: state, collection_batch_id: 3, capture_batch_id: 3, latest_readings: latest }, format);
    assert.equal(cold.collection_id, 3);assert.equal(nodeCardView(cold).temp, '--');
    cold = mergeVibrationDetails(cold, saved, format);assert.equal(nodeCardView(cold).temp, '--');
    cold = mergeVibrationDetails(cold, descriptor, format);assert.equal(nodeCardView(cold).waveform_available, false);
    cold = mergeVibrationDetails(cold, { ...descriptor, upload_started: true, collection_values: { temperature: 32 } }, format);
    assert.equal(nodeCardView(cold).temp, '32.00');assert.equal(nodeCardView(cold).digital_db, '--');
}
const pendingAfterOld = mergeDeviceSnapshot(undefined, { mac: 'one', collection_state: 'complete', collection_batch_id: 100, capture_batch_id: 3, latest_readings: latest }, format);
assert.equal(pendingAfterOld.collection_id, 3);assert.equal(nodeCardView(pendingAfterOld).temp, '--');
const untouched = mergeDeviceSnapshot(undefined, { mac: 'other', status: 'Offline', collection_state: 'idle', latest_readings: latest }, format);
assert.equal(untouched.temp, '24.50');assert.equal(untouched.status, 'Offline');
assert.equal(nodeCardView(previous).temp, '24.50'); // Other cards and history are not mutated.
console.log('Node cycle display PASS: empty capture/queue, immediate partial values, zero/null, exact batch, cold reload, stale HTTP, split completion and overlapping acquisition');
const finishedWithoutNext = mergeDeviceSnapshot(undefined, { mac: 'one', collection_state: 'complete', collection_batch_id: 3, capture_batch_id: 100, capture_pending: false, latest_readings: latest }, format);
assert.equal(finishedWithoutNext.pending_collection_id, undefined);
const overlapReload = mergeDeviceSnapshot(undefined, { mac: 'one', collection_state: 'uploading', collection_batch_id: 100, capture_batch_id: 3, capture_pending: true, latest_readings: latest }, format);
assert.equal(overlapReload.collection_id, 100);assert.equal(overlapReload.pending_collection_id, 3);
const finishedOverlap = mergeVibrationDetails(overlapReload, saved, format);
assert.equal(nodeCardView(finishedOverlap).collection_id, 3);assert.equal(nodeCardView(finishedOverlap).temp, '--');
const afterError = mergeLiveCollection({ ...previous, collection_state: 'error', raw_upload_status: 'uploading', vibration_features_ready: false }, { type: 'monitor_update', batch_id: 5, collection_state: 'capturing' });
assert.equal(afterError.collection_id, 5);assert.equal(nodeCardView(afterError).temp, '--');

// Capture time belongs to the exact batch and survives queueing/late receipt.
let timed = mergeLiveCollection({}, { type: 'monitor_update', batch_id: 90,
    collection_state: 'capturing', captured_at: 1000 }, format);
assert.equal(nodeCardView(timed).capture_time, 'time:1000');
timed = mergeLiveCollection(timed, { type: 'vibration_upload_progress', batch_id: 90, status: 'queued' }, format);
assert.equal(nodeCardView(timed).capture_time, 'time:1000');
timed = mergeVibrationDetails(timed, { ...saved, batch_id: 90, captured_at: 1000, timestamp: 9000 }, format);
assert.equal(nodeCardView(timed).capture_time, 'time:1000');
assert.equal(mergeLiveCollection(timed, { type: 'monitor_update', batch_id: 90,
    collection_state: 'complete', captured_at: 8000 }, format).capture_time, 'time:1000');
const freshTime = mergeLiveCollection(timed, { type: 'monitor_update', batch_id: 91,
    collection_state: 'capturing', captured_at: 2000 }, format);
assert.equal(nodeCardView(freshTime).capture_time, 'time:2000');
const coldTime = mergeDeviceSnapshot(undefined, { collection_batch_id: 91, capture_batch_id: 91,
    capture_pending: true, collection_state: 'waiting', captured_at: 2000, pending_captured_at: 2000 }, format);
assert.equal(nodeCardView(coldTime).capture_time, 'time:2000');
assert.equal(nodeCardView(mergeVibrationDetails({}, saved, format)).capture_time, '');

const onWire = { ...timed, raw_upload_status: 'uploading', vibration_features_ready: false,
    collection_state: 'uploading', upload_started: true };
const overlap = mergeLiveCollection(onWire, { type: 'monitor_update', batch_id: 92,
    collection_state: 'capturing', captured_at: 3000 }, format);
assert.equal(nodeCardView(overlap).capture_time, 'time:1000');
const finishedOld = mergeVibrationDetails(overlap, { ...saved, batch_id: 90,
    captured_at: 1000, timestamp: 9500 }, format);
assert.equal(nodeCardView(finishedOld).collection_id, 92);
assert.equal(nodeCardView(finishedOld).capture_time, 'time:3000');
