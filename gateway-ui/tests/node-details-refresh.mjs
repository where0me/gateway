import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { mergeDeviceSnapshot, mergeLiveCollection, mergeVibrationDetails } from '../src/nodeDetailsState.js';
import { createMonitoringSwitchState } from '../src/monitoringSwitchState.js';

// Run the actual App callbacks, poll effect and WebSocket handler together.
const source = readFileSync(new URL('../src/App.jsx', import.meta.url), 'utf8');
const callbacks = source.slice(source.indexOf('    const vibrationRequests ='), source.indexOf('    const samplingConfigToForm ='));
const pollStart = source.indexOf('    useEffect(() => {', source.indexOf('/* 无RTC网关'));
const pollEnd = source.indexOf('}, [checkGatewayClock, loadLatestVibration]);', pollStart);
const poll = source.slice(pollStart, pollEnd + '}, [checkGatewayClock, loadLatestVibration]);'.length);
const wsStart = source.indexOf('    useEffect(() => {', source.indexOf('/* WebSocket */'));
const wsSuffix = '}, [applyChannelOperationResult, refreshDeviceDetails, recordHomeActivity, loadLatestVibration]);';
const ws = source.slice(wsStart, source.indexOf(wsSuffix, wsStart) + wsSuffix.length);
assert.ok(callbacks && pollEnd > pollStart && ws.length);
const flush = async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); };

function harness() {
    let devices = [], interval;
    const pending = [], cleanups = [], sockets = [], errors = [];
    const noop = () => {};
    class Socket {
        static CONNECTING = 0; static OPEN = 1;
        constructor() { this.readyState = 1; sockets.push(this); }
        close() {}
    }
    const context = vm.createContext({
        useRef: value => ({ current: value }), useCallback: callback => callback,
        useEffect: callback => cleanups.push(callback()),
        fetch(url) { return new Promise(resolve => pending.push({ url, resolve })); },
        URLSearchParams, Map, Promise, WebSocket: Socket, TEMP_LIMIT: 45,
        MIN_VALID_GATEWAY_TIME_MS: 0, TEMP_HISTORY_TIME: 1800000,
        window: { location: { hostname: 'test' }, setInterval(callback, ms) {
            assert.equal(ms, 3000); interval = callback; return 1;
        }, clearInterval: noop },
        console: { log: noop, error: (...args) => errors.push(args) },
        formatCollectionTime: time => time ? `time:${time}` : '',
        buildDefaultDeviceName: () => 'node', collectionStateText: state => state,
        mergeDeviceSnapshot, mergeLiveCollection, mergeVibrationDetails,
        monitorSwitches: { current: createMonitoringSwitchState() },
        checkGatewayClock: () => Promise.resolve(), applyChannelOperationResult: noop,
        recordHomeActivity: noop, setNodeLastUplink: noop, setNodeMonitoring: noop,
        setCollectionStatus: noop, setTempHistory: noop, setSoundHistory: noop, setTrendWindowEnd: noop,
        setTimeout: noop, clearTimeout: noop,
        setWhitelist(update) { devices = typeof update === 'function' ? update(devices) : update; }
    });
    vm.runInContext(callbacks + '\n' + poll + '\n' + ws, context);
    return {
        async respond(kind, data, status = 200) {
            const index = pending.findIndex(request => request.url.includes(kind));
            assert.notEqual(index, -1, `Missing request ${kind}`);
            pending.splice(index, 1)[0].resolve({ ok: status === 200, status, json: async () => data });
            await flush();
        },
        emit(event) { sockets[0].onmessage({ data: JSON.stringify(event) }); },
        tick() { interval(); },
        stop() { cleanups.forEach(cleanup => cleanup?.()); },
        devices: () => devices, pending, errors
    };
}

const details = { status: 'success', batch_id: 100, sample_rate_hz: 26667, sample_count: 13334,
    timestamp: 1000, features_ready: false, features: [], raw_available: false,
    upload_started: false, raw_received_packets: 0, raw_total_packets: 27, raw_progress_unit: 'blocks' };
const complete = { ...details, raw_available: true, upload_started: true, features_ready: true,
    features: [[1,2,3,4,5], [0,0,0,0,0], [6,7,8,9,10]], collection_values: { temperature: 26.19, digital_db: 66, analog_db: 64 } };
const list = (batch, state) => [{ mac: 'one', name: 'node', status: 'Online', collection_batch_id: batch,
    capture_batch_id: batch, collection_state: state, temp: '20', latest_readings: { temperature: 26.19, features: complete.features, features_batch_id: 100, timestamp: 1000 } }];
async function settle(h, nodes, data) {
    for (let i=0;i<20 && h.pending.length;i++) {
        if(h.pending.some(p=>p.url.includes('/api/device/list'))) await h.respond('/api/device/list',nodes);
        else await h.respond('/vibration/latest',data);
    }
}
const h = harness();await flush();await settle(h,list(100,'complete'),complete);
assert.equal(h.devices()[0].temp,'26.19');assert.equal(h.devices()[0].vibration_features_ready,true);
// A request initiated before a live capture cannot put the old values back.
h.tick();h.emit({type:'monitor_update',mac:'one',batch_id:3,collection_state:'capturing'});
await h.respond('/api/device/list',list(100,'complete'));
assert.equal(h.devices()[0].collection_id,3);assert.equal(h.devices()[0].temp,'--');
assert.equal(h.devices()[0].vibration_features.length,0);
h.emit({type:'vibration_upload_progress',mac:'one',batch_id:3,status:'queued',received_packets:0,total_packets:27});
assert.equal(h.devices()[0].collection_state,'waiting');
assert.equal(h.devices()[0].raw_upload_status,'queued');assert.equal(h.devices()[0].upload_started,false);
h.emit({type:'monitor_update',mac:'one',batch_id:3,collection_state:'capturing'});
assert.equal(h.devices()[0].collection_state,'waiting');
h.tick();await h.respond('/api/device/list',list(3,'capturing'));
assert.match(h.pending[0].url,/batch=3/);
await h.respond('/vibration/latest',{...details,batch_id:3,last_complete:complete});
assert.equal(h.devices()[0].temp,'--');
assert.equal(h.devices()[0].collection_state,'waiting');
// Upload notification refreshes the exact batch without waiting for a poll.
h.emit({type:'monitor_update',mac:'one',batch_id:3,collection_state:'uploading'});
assert.ok(h.pending.some(p=>p.url.includes('/api/device/list')));
await settle(h,list(3,'uploading'),{...details,batch_id:3,upload_started:true,collection_values:{temperature:32}});
assert.equal(h.devices()[0].temp,'32.00');assert.equal(h.devices()[0].digital_db,'--');
h.emit({type:'temp_update',mac:'one',batch_id:3,temp:0,timestamp:1100});
h.emit({type:'sound_update',mac:'one',batch_id:3,source:'digital',db:0,timestamp:1100});
h.emit({type:'sound_update',mac:'one',batch_id:3,source:'analog',db:65,timestamp:1100});
assert.equal(h.devices()[0].temp,'0.00');assert.equal(h.devices()[0].digital_db,'0.00');
assert.equal(h.devices()[0].analog_db,'65.00');
h.emit({type:'vibration_upload_progress',mac:'one',batch_id:3,received_packets:1,total_packets:27,unit:'blocks'});
assert.ok(h.pending.some(p=>p.url.includes('batch=3')));
await h.respond('/vibration/latest',{...details,batch_id:3,upload_started:true,raw_received_packets:1,collection_values:{temperature:99,digital_db:99}});
assert.equal(h.devices()[0].temp,'0.00');assert.equal(h.devices()[0].digital_db,'0.00');
// Missing the completion WebSocket still converges from the HTTP fallback.
h.tick();await settle(h,list(3,'uploading'),{...complete,batch_id:3});
assert.equal(h.devices()[0].raw_upload_status,'complete');assert.equal(h.devices()[0].vibration_features_ready,true);
h.tick();await h.respond('/api/device/list',list(3,'complete'));
const before=h.devices();h.stop();await h.respond('/vibration/latest',{...complete,batch_id:4});
assert.equal(h.devices(),before);assert.equal(h.errors.length,0,JSON.stringify(h.errors));assert.equal(h.pending.length,0);
const many=harness();await flush();
await many.respond('/api/device/list',[...list(3,'capturing'),{mac:'other',status:'Offline',collection_state:'idle',latest_readings:{temperature:24}}]);
await many.respond('mac=one',{...details,batch_id:3});await many.respond('mac=other',{},404);
assert.equal(many.devices()[0].temp,'--');assert.equal(many.devices()[1].temp,'24.00');
many.emit({type:'temp_update',mac:'one',batch_id:3,temp:33});
assert.equal(many.devices()[0].temp,'33.00');assert.equal(many.devices()[1].temp,'24.00');
many.stop();assert.equal(many.errors.length,0);
console.log('Actual App cycle pipeline PASS: capture reset, stale-list protection, pinned immediate upload refresh, partial live updates, missed completion fallback, isolation and unmount');
