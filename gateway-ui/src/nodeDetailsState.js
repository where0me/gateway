// Once acquisition starts, the card belongs to that cycle. Historical values
// remain archived but cannot fill holes in a pending or partially received batch.
const scalarFields = ['temp', 'mag', 'digital_db', 'analog_db'];
const isNumber = value => value !== null && value !== undefined && value !== '' && Number.isFinite(Number(value));
const sameBatch = (device, batch) => isNumber(device.collection_id) && Number(device.collection_id) === Number(batch);
const activeUpload = device => device?.upload_started && !batchComplete(device) && device.collection_state !== 'error';

function readingFields(values = {}) {
    const fields = {};
    for (const [field, source] of [['temp', 'temperature'], ['digital_db', 'digital_db'], ['analog_db', 'analog_db']]) {
        if (isNumber(values[source])) fields[field] = Number(values[source]).toFixed(2);
    }
    const mag = values.magnetic;
    if (mag && ['x', 'y', 'z'].every(axis => isNumber(mag[axis])))
        fields.mag = `X:${Number(mag.x).toFixed(1)}, Y:${Number(mag.y).toFixed(1)}, Z:${Number(mag.z).toFixed(1)}`;
    return fields;
}

function withCaptureTime(device, batch, capturedAt, formatTime) {
    const stamp = Number(capturedAt);
    if (!Number.isFinite(stamp) || stamp <= 0 || !isNumber(batch)) return device;
    if (isNumber(device.collection_id) && !sameBatch(device, batch)) {
        if (Number(device.pending_collection_id) !== Number(batch) || device.pending_capture_time) return device;
        return { ...device, pending_capture_time: formatTime(stamp), pending_capture_time_at: stamp };
    }
    if (device.capture_time && Number(device.capture_time_batch_id) === Number(batch)) return device;
    return { ...device, capture_time: formatTime(stamp), capture_time_at: stamp, capture_time_batch_id: Number(batch) };
}

export function mergeDeviceSnapshot(previous, snapshot, formatTime = time => time) {
    // Queue admission is later evidence than the same batch's capture report.
    if (sameBatch(previous || {}, snapshot.collection_batch_id) && previous?.raw_upload_status === 'queued' &&
        snapshot.collection_state === 'capturing') snapshot = { ...snapshot, collection_state: 'waiting' };
    let result = { ...previous, ...snapshot };
    const recorded = Number(snapshot.collection_batch_id) || 0;
    const captured = Number(snapshot.capture_batch_id) || 0;
    const pendingCapture = snapshot.collection_state === 'waiting' && recorded &&
        recorded !== Number(snapshot.latest_readings?.features_batch_id);
    if (recorded && (['capturing', 'uploading'].includes(snapshot.collection_state) || pendingCapture)) {
        // Live progress can be ahead of the persisted state notification.
        if (!(activeUpload(previous) && sameBatch(previous, recorded)))
            result = { ...result, ...mergeLiveCollection(previous ? { ...result, collection_state: previous.collection_state } : result,
                { type: 'monitor_update', batch_id: recorded, collection_state: pendingCapture ? 'capturing' : snapshot.collection_state }, formatTime) };
        if (pendingCapture && !result.upload_started) result.collection_state = 'waiting';
    }
    if (captured && !sameBatch(result, captured) && snapshot.capture_pending !== false) {
        if (activeUpload(result)) result.pending_collection_id = captured;
        else result = { ...selectBatch(result, captured), collection_state: 'waiting' };
    }
    // Older servers do not expose a batch ID. Still hide history on a cold
    // reload while capturing; a live event/descriptor will identify this batch.
    if (!recorded && snapshot.collection_state === 'capturing' && !previous?.cycle_active) {
        result = { ...selectBatch(result, undefined, true), collection_state: 'capturing',
            blocked_batch_id: previous?.collection_id || snapshot.latest_readings?.features_batch_id };
    }
    if (activeUpload(previous) && sameBatch(previous, result.collection_id))
        result.collection_state = previous.collection_state;
    const latest = snapshot.latest_readings || {};
    const persisted = readingFields(latest);
    for (const field of scalarFields) {
        if (result.cycle_active) {
            result[field] = previous?.cycle_active &&
                (sameBatch(previous, result.collection_id) || (!isNumber(previous.collection_id) && !isNumber(result.collection_id)))
                ? previous[field] ?? '--' : result[field] ?? '--';
            continue;
        }
        // Live fields from this batch win over an HTTP snapshot captured earlier.
        const cached = previous?.[field];
        result[field] = previous?.live_fields?.[field] && cached != null ? cached
            : persisted[field] ?? (cached != null && cached !== '--' ? cached : snapshot[field]);
    }
    if (!result.cycle_active && Array.isArray(latest.features) && latest.features.length === 3 &&
        !(previous?.vibration_features_ready && sameBatch(previous, previous.vibration_features_batch_id))) {
        result.vibration_features = latest.features;
        result.vibration_features_batch_id = latest.features_batch_id;
        result.vibration_features_timestamp = latest.features_timestamp;
    }
    if (!result.cycle_active && Number(latest.timestamp) >= (previous?.last_reading_at || 0)) {
        result.last_reading_at = Number(latest.timestamp) || 0;
        result.last_reading_time = formatTime(latest.timestamp) || previous?.last_reading_time;
    }
    if (!result.cycle_active) result.last_sample_time = previous?.last_sample_time || snapshot.last_sample_time;
    else if (previous?.cycle_active && sameBatch(previous, result.collection_id)) result.last_sample_time = previous.last_sample_time;
    result.is_alarm = isNumber(result.temp) && Number(result.temp) > 45;
    const timeBatch = sameBatch(result, captured) && snapshot.capture_pending !== false ? captured : recorded;
    result = withCaptureTime(result, timeBatch,
        timeBatch === captured ? snapshot.pending_captured_at : snapshot.captured_at, formatTime);
    if (captured && Number(result.pending_collection_id) === captured)
        result = withCaptureTime(result, captured, snapshot.pending_captured_at, formatTime);
    if (!result.cycle_active) result = withCaptureTime(result, latest.batch_id, latest.captured_at, formatTime);
    return result;
}

function selectBatch(device, batch, unknown = false) {
    if (!unknown && (!isNumber(batch) || Number(batch) <= 0 || sameBatch(device, batch))) return device;
    const pendingTime = Number(device.pending_collection_id) === Number(batch) && device.pending_capture_time;
    const result = { ...device, ...Object.fromEntries(scalarFields.map(field => [field, '--'])),
        collection_id: unknown ? undefined : Number(batch), cycle_active: true, awaiting_vibration_batch: true,
        raw_upload_status: 'waiting', raw_received_packets: 0, raw_total_packets: 0,
        raw_progress_unit: 'packets', vibration_samples: [], vibration_sample_count: 0,
        vibration_features: [], vibration_features_ready: false, vibration_features_batch_id: undefined,
        vibration_features_timestamp: 0, last_reading_at: 0, last_reading_time: '', last_sample_time: '',
        capture_time: pendingTime || '', capture_time_at: pendingTime ? device.pending_capture_time_at : 0,
        capture_time_batch_id: pendingTime ? Number(batch) : undefined,
        pending_capture_time: '', pending_capture_time_at: 0,
        pending_collection_id: undefined, upload_started: false, batch_values: {}, live_fields: {}, is_alarm: false };
    return result;
}

export function mergeLiveCollection(device, event, formatTime = time => new Date(time * 1000).toLocaleString()) {
    const result = mergeLiveCollectionData(device, event, formatTime);
    return withCaptureTime(result, event.batch_id, event.captured_at, formatTime);
}

function mergeLiveCollectionData(device, event, formatTime) {
    if (device.cycle_active && !sameBatch(device, event.batch_id) &&
        (['complete', 'error'].includes(event.collection_state) ||
         (event.type === 'collection_data_ready' && event.raw_complete))) return device;
    // Acquisition of the next batch can overlap an older upload. Do not replace
    // the batch currently on the wire with a queued/capturing batch.
    const waiting = event.collection_state === 'capturing' || event.status === 'queued';
    if (event.collection_state === 'capturing' && sameBatch(device, event.batch_id) &&
        device.raw_upload_status === 'queued') return device;
    if (event.status === 'queued' && sameBatch(device, event.batch_id) && batchComplete(device)) return device;
    if (event.collection_state === 'capturing' && activeUpload(device) && sameBatch(device, event.batch_id)) return device;
    if (waiting && activeUpload(device) && !sameBatch(device, event.batch_id))
        return { ...device, pending_collection_id: event.batch_id };
    // This notification also acknowledges an empty descriptor at grant time.
    if (event.type === 'collection_data_ready' && !event.raw_complete) return device;
    const businessState = ['capturing', 'uploading', 'complete', 'error'].includes(event.collection_state);
    // Idle/waiting telemetry may contain batch 0, not a new collection.
    const select = event.type !== 'monitor_update' || businessState;
    const next = select ? selectBatch(device, event.batch_id) : device;
    const result = { ...next, status: 'Online' };
    if (waiting) result.upload_started = false;
    if (['temp_update', 'mag_update', 'sound_update'].includes(event.type) ||
        event.collection_state === 'uploading' ||
        (event.type === 'vibration_upload_progress' && event.status !== 'queued') || event.raw_complete) {
        result.upload_started = true;
        if (!batchComplete(result)) {
            result.collection_state = 'uploading';
            if (['waiting', 'queued'].includes(result.raw_upload_status)) result.raw_upload_status = 'uploading';
        }
    }
    if (event.type === 'temp_update' && isNumber(event.temp)) {
        result.temp = Number(event.temp).toFixed(2);
        result.is_alarm = Number(event.temp) > 45;
        result.live_fields = { ...result.live_fields, temp: true };
    } else if (event.type === 'mag_update' && event.mag != null && event.mag !== '--') {
        result.mag = event.mag;
        result.live_fields = { ...result.live_fields, mag: true };
    }
    else if (event.type === 'sound_update' && isNumber(event.db)) {
        const field = event.source === 'digital' ? 'digital_db' : 'analog_db';
        result[field] = Number(event.db).toFixed(2);
        result.live_fields = { ...result.live_fields, [field]: true };
    } else if (event.type === 'monitor_update') {
        if (!(event.collection_state === 'capturing' && result.upload_started && sameBatch(device, event.batch_id)))
            result.collection_state = event.collection_state;
    } else if (event.type === 'vibration_upload_progress') {
        // A late progress packet must not turn an already committed waveform back into uploading.
        if (result.raw_upload_status !== 'complete') {
            result.collection_state = event.status === 'queued' ? 'waiting' : 'uploading';
            result.raw_upload_status = event.status || 'uploading';
            result.raw_received_packets = Number(event.received_packets) || 0;
            result.raw_total_packets = Number(event.total_packets) || 0;
            result.raw_progress_unit = event.unit || 'packets';
        }
    } else if (event.type === 'collection_data_ready' && event.raw_complete) {
        result.raw_upload_status = 'complete';
    }
    if (['temp_update', 'mag_update', 'sound_update'].includes(event.type)) {
        const stamp = Number(event.timestamp) > 0 ? Number(event.timestamp) : Math.floor(Date.now() / 1000);
        result.last_reading_at = Math.max(result.last_reading_at || 0, stamp);
        result.last_reading_time = formatTime(result.last_reading_at);
    }
    return result;
}

export function mergeVibrationDetails(device, data, formatTime) {
    if (!isNumber(data.batch_id)) return device;
    // The API supplies a durable fallback even after a browser reload while a
    // new descriptor is waiting for its first sensor packet.
    let lastComplete = device.last_complete;
    if (data.last_complete) lastComplete = completedSnapshot(data.last_complete, formatTime);
    if (data.raw_available && data.features_ready) lastComplete = completedSnapshot(data, formatTime);
    // A request made just after a new capture may still see the preceding DB row.
    if (device.awaiting_vibration_batch && !sameBatch(device, data.batch_id))
        if (isNumber(device.collection_id) || Number(device.blocked_batch_id) === Number(data.batch_id) ||
            (device.collection_state === 'capturing' && data.raw_available)) return device;
    if (device.cycle_active && isNumber(device.collection_id) && !sameBatch(device, data.batch_id)) return device;
    const matched = sameBatch(device, data.batch_id);
    const next = selectBatch(device, data.batch_id);
    const rate = Number(data.sample_rate_hz) || 1;
    const step = Number(data.display_step) || 1;
    const features = Array.isArray(data.features) ? data.features : [];
    const ready = data.features_ready ?? features.length === 3;
    const values = data.collection_values || {};
    const result = { ...next, awaiting_vibration_batch: false,
        last_complete: lastComplete,
        upload_started: (matched && device.upload_started) ||
            (data.upload_started ?? (data.raw_available || Number(data.raw_received_packets) > 0 ||
                Object.keys(data.collection_values || {}).length > 0)),
        batch_values: { ...next.batch_values, ...readingFields(values) },
        raw_upload_status: data.raw_available || (matched && device.raw_upload_status === 'complete')
            ? 'complete' : data.raw_upload_status || 'uploading',
        raw_received_packets: data.raw_received_packets ?? next.raw_received_packets ?? 0,
        raw_total_packets: data.raw_total_packets ?? next.raw_total_packets ?? 0,
        raw_progress_unit: data.raw_progress_unit || next.raw_progress_unit || 'packets',
        vibration_sampling_rate: rate, vibration_sample_count: Number(data.sample_count) || 0,
        vibration_features_ready: ready || next.vibration_features_ready || false,
        vibration_features: ready ? features : next.vibration_features || [],
        vibration_features_batch_id: ready ? data.batch_id : next.vibration_features_batch_id,
        vibration_features_timestamp: ready ? data.timestamp : next.vibration_features_timestamp,
        last_sample_time: data.upload_started || data.raw_available || ready ? formatTime(data.timestamp) || next.last_sample_time : next.last_sample_time };
    // The latest waveform can remain on an older batch for several polls.
    // Keep newer live/per-sensor readings rather than restoring its old values.
    for (const [field, value] of Object.entries(readingFields(values))) {
        if (matched && device.live_fields?.[field]) continue;
        result[field] = value;
    }
    if (Object.keys(readingFields(values)).length || ready || data.raw_available) {
        const timestamp = Math.max(result.last_reading_at || 0, Number(data.timestamp) || 0);
        result.last_reading_at = timestamp;
        result.last_reading_time = timestamp ? formatTime(timestamp) : '';
    }
    result.is_alarm = isNumber(result.temp) && Number(result.temp) > 45;
    if (Array.isArray(data.samples) && data.samples.length &&
        (!matched || data.samples.length >= (next.vibration_samples?.length || 0))) {
        result.vibration_samples = data.samples.map((sample, index) => ({ index: index * step,
            time: index * step / rate, x_raw: sample[0], y_raw: sample[1], z_raw: sample[2],
            x: sample[0] * 0.000488, y: sample[1] * 0.000488, z: sample[2] * 0.000488 }));
    }
    if (batchComplete(result)) result.collection_state = 'complete';
    return withCaptureTime(result, data.batch_id, data.captured_at, formatTime);
}

function batchComplete(device) {
    return device.raw_upload_status === 'complete' && device.vibration_features_ready === true;
}

function completedSnapshot(data, formatTime) {
    return { ...Object.fromEntries(scalarFields.map(field => [field, '--'])),
        ...readingFields(data.collection_values), collection_id: Number(data.batch_id),
        raw_upload_status: 'complete', vibration_features_ready: true,
        capture_time: Number(data.captured_at) > 0 ? formatTime(data.captured_at) : '',
        capture_time_at: Number(data.captured_at) || 0, capture_time_batch_id: Number(data.batch_id),
        vibration_features: data.features, vibration_features_timestamp: data.timestamp,
        last_reading_time: formatTime(data.timestamp), last_sample_time: formatTime(data.timestamp) };
}

// A card displays one coherent batch; current radio/power/online telemetry is
// independent. Never pair the last waveform's features with a new scalar batch.
export function nodeCardView(device) {
    if (device.pending_collection_id && !sameBatch(device, device.pending_collection_id) && batchComplete(device))
        device = { ...selectBatch(device, device.pending_collection_id), collection_state: 'waiting' };
    const uploading = device.upload_started === true && !batchComplete(device);
    const pending = !device.upload_started && (device.cycle_active || device.collection_state === 'capturing' ||
        ['waiting', 'queued'].includes(device.raw_upload_status));
    let view = device;
    if (pending) view = { ...device, ...Object.fromEntries(scalarFields.map(field => [field, '--'])),
        vibration_features: [], vibration_features_ready: false, raw_upload_status: 'waiting',
        vibration_features_timestamp: 0, last_reading_time: '', last_sample_time: '' };
    else if (!device.cycle_active && !uploading && device.last_complete) view = { ...device, ...device.last_complete };
    else if (uploading) {
        view = { ...device, ...Object.fromEntries(scalarFields.map(field => [field,
            device.live_fields?.[field] ? device[field] : device.batch_values?.[field] ?? '--'])),
            vibration_features: device.vibration_features_ready ? device.vibration_features : [] };
    }
    return { ...view, is_alarm: isNumber(view.temp) && Number(view.temp) > 45,
        card_uploading: uploading, card_pending: Boolean(pending), card_batch_active: Boolean(device.cycle_active),
        waveform_available: !pending && !uploading && batchComplete(view) };
}

export function uploadStatusText(device) {
    if (device.collection_state === 'error') return '采集或上传异常，等待重试';
    if (device.pending_collection_id && !sameBatch(device, device.pending_collection_id) && batchComplete(device))
        return '等待数据上传';
    if (device.raw_upload_status === 'queued') return '等待数据上传';
    if (device.collection_state === 'capturing' && !device.upload_started) return '正在采集传感器数据';
    if (device.upload_started === false) return '等待数据上传';
    if (device.raw_upload_status !== 'complete' && device.raw_total_packets) {
        const total = Number(device.raw_total_packets);
        const received = Math.min(total, Math.max(0, Number(device.raw_received_packets) || 0));
        const unit = device.raw_progress_unit === 'blocks' ? '窗口' : '包';
        return `${device.raw_upload_status === 'queued' ? '排队等待上传' : '上传中'} ${received}/${total} ${unit}（${Math.floor(received / total * 100)}%）`;
    }
    if (device.raw_upload_status === 'complete') return device.vibration_features_ready === false
        ? '原始波形已接收，等待振动特征' : '● 原始波形上传完成';
    if (device.vibration_features_ready) return '振动特征已接收，暂无原始波形';
    return device.upload_started ? '正在上传，等待波形数据' : '等待数据上传';
}
