import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// Exercise the actual card/window loader: a newer upload must not change the
// batch downloaded by a waiting card or an already opened waveform window.
const source = readFileSync(new URL('../src/NodeDetails.jsx', import.meta.url), 'utf8');
const loader = source.slice(source.indexOf('async function readWaveform('), source.indexOf('function downloadRaw('));
let requested;
let payload = { status: 'success', batch_id: 100, raw_available: true, display_step: 1,
    sample_rate_hz: 1000, sample_count: 2, samples: [[1, 2, 3], [4, 5, 6]] };
const context = vm.createContext({ URLSearchParams, window: { location: { hostname: 'gateway' } },
    async fetch(url) { requested = new URL(url); return { ok: true, async json() { return payload; } }; } });
vm.runInContext(loader, context);
const result = await context.readWaveform({ mac: 'one', collection_id: 100 });
assert.equal(requested.searchParams.get('batch'), '100');
assert.equal(requested.searchParams.get('mac'), 'one');
assert.equal(requested.searchParams.get('raw'), '1');
assert.equal(result.vibration_samples.length, 2);
assert.equal(result.vibration_samples[1].x_raw, 4);
payload = { ...payload, batch_id: 3 };
await assert.rejects(() => context.readWaveform({ mac: 'one', collection_id: 100 }), /批次已更新/);
payload = { ...payload, batch_id: 100, samples: [[1, 2, 3]] };
await assert.rejects(() => context.readWaveform({ mac: 'one', collection_id: 100 }), /数据不完整/);
console.log('Waveform actions: pinned batch for download/window and incomplete/mismatched data rejection PASS');
