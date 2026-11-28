import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// Run the real manual registration handler: a successful registration must
// never manufacture a visible node before the server confirms its join.
const source = readFileSync(new URL('../src/App.jsx', import.meta.url), 'utf8');
const handler = source.match(/    const handleManualAdd = \(\) => \{[\s\S]*?^    \};/m)?.[0];
assert.ok(handler);
async function register(status, input = '01:02:03:04:05:06:07:08') {
    let nodes = [{ mac: 'AA:BB:CC:DD:EE:FF:00:11', name: 'Already joined' }];
    const alerts = [], writes = [];
    const context = {
        manualMac: input, window: { location: { hostname: 'gateway' } },
        alert: message => alerts.push(message), console,
        setWhitelist: update => { nodes = update(nodes); },
        setNodeMonitoring() {}, setCollectionStatus() {}, setManualMac() {},
        fetch: async (url, options) => {
            writes.push(JSON.parse(options.body));
            return { json: async () => ({ status }) };
        }
    };
    vm.runInNewContext(`${handler}\nhandleManualAdd();`, context);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(nodes.length, 1);
    assert.equal(nodes[0].mac, 'AA:BB:CC:DD:EE:FF:00:11');
    return { alerts, writes };
}
assert.match((await register('success')).alerts[0], /完成入网/);
assert.match((await register('exists')).alerts[0], /已登记/);
assert.equal((await register('success', 'bad MAC')).writes.length, 0);
console.log('PASS: manual registration waits for server-confirmed visibility and preserves joined nodes');
