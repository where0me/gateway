import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { createMonitoringSwitchState } from '../src/monitoringSwitchState.js';

// Exercise the actual click handler with delayed HTTP replies and stale polls.
const source = readFileSync(new URL('../src/App.jsx', import.meta.url), 'utf8');
const start = source.indexOf('    async function changeMonitoring(');
const end = source.indexOf('    const [activeTab,', start);
assert.ok(start > 0 && end > start);
assert.ok(source.includes('checked={monitorSwitches.current.value(dev)}'));
const controller = createMonitoringSwitchState();
const requests = [], alerts = [];
let devices = [
    { mac: 'one', online_monitoring_enabled: false },
    { mac: 'two', online_monitoring_enabled: false }
], saving = {};
const context = vm.createContext({
    monitorSwitches: { current: controller },
    setMonitorSaving(update) { saving = update(saving); },
    setWhitelist(update) { devices = update(devices); },
    fetch(url, options) {
        return new Promise((resolve, reject) => requests.push({ url, options, resolve, reject }));
    },
    window: { location: { hostname: 'test' }, alert(message) { alerts.push(message); } }
});
vm.runInContext(source.slice(start, end), context);
const current = mac => devices.find(device => device.mac === mac);
const visible = mac => controller.value(current(mac));
const reply = (index, data, ok = true) => requests[index].resolve({ ok, json: async () => data });
const applyPoll = (mac, enabled, version) => {
    devices = devices.map(device => device.mac === mac
        ? controller.snapshot({ ...device, online_monitoring_enabled: enabled }, version) : device);
};

const beforeClick = controller.version();
const first = context.changeMonitoring(current('one'), true);
assert.equal(visible('one'), true); // Before any response or timer.
assert.equal(current('one').online_monitoring_enabled, true);
assert.equal(saving.one, true);
assert.equal(visible('two'), false);
assert.deepEqual(JSON.parse(requests[0].options.body), { mac: 'one', enabled: true });
await context.changeMonitoring(current('one'), false);
assert.equal(requests.length, 1); // Double click cannot race two saves.
applyPoll('one', false, beforeClick);
assert.equal(visible('one'), true);
const duringSave = controller.version();
reply(0, { status: 'success', enabled: true }); await first;
assert.equal(saving.one, false);
applyPoll('one', false, duringSave);
assert.equal(visible('one'), true); // A pre-save GET can arrive after POST.
applyPoll('one', true, controller.version());
assert.equal(visible('one'), true);
applyPoll('one', false, beforeClick);
assert.equal(visible('one'), true); // Older GET arrives after a newer confirmation.
// A later authoritative change from another client is still respected.
applyPoll('one', false, controller.version());assert.equal(visible('one'), false);

const a = context.changeMonitoring(current('one'), true);
const b = context.changeMonitoring(current('two'), true);
assert.equal(visible('one'), true);assert.equal(visible('two'), true);
reply(2, { status: 'success', enabled: true });await b;
reply(1, { status: 'error', msg: 'database busy' }, false);await a;
assert.equal(visible('one'), false);assert.equal(visible('two'), true);
assert.equal(saving.one, false);assert.equal(saving.two, false);
assert.match(alerts[0], /database busy/);

// Roll back to the previous displayed intent, even before a fresh GET confirms it.
const off = context.changeMonitoring(current('two'), false);
assert.equal(visible('two'), false);
requests[3].reject(new Error('network disconnected'));await off;
assert.equal(visible('two'), true);assert.equal(saving.two, false);
assert.match(alerts[1], /network disconnected/);
console.log('Actual monitoring switch: immediate ON/OFF, pending guards, stale polls, server reconciliation, independent nodes and failure rollback PASS');
