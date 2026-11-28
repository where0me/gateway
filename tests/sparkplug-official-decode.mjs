import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';
const dir = resolve(process.argv[2] || 'out/sparkplug-tests');
const require = createRequire(resolve(dir, 'decoder/package.json'));
const protobuf = require('protobufjs');
const root = await protobuf.load(resolve(dir, 'sparkplug_b.proto'));
const Payload = root.lookupType('org.eclipse.tahu.protobuf.Payload');
async function decode(name) {
    const bytes = await readFile(resolve(dir, `${name}.pb`));
    const value = Payload.toObject(Payload.decode(bytes), { longs: Number, bytes: Buffer });
    assert.equal(Payload.verify(value), null);
    return value;
}
const birth = await decode('dbirth'), schema = new Map(birth.metrics.map(m => [m.alias, m]));
assert.equal(birth.metrics.length, 37);
assert.equal(schema.get(18).name, 'Temperature');assert.equal(schema.get(18).datatype, 10);
assert.equal(schema.get(18).properties.keys[0], 'engUnit');
assert.equal(schema.get(18).properties.values[0].stringValue, 'C');
assert.equal(schema.get(46).datatype, 17);assert.equal(schema.get(46).isNull, true);
assert.equal(schema.get(18).doubleValue, 27.5);
for (const [file, batch, historical] of [['collection-historical', 42, true], ['collection-current', 43, false]]) {
    const message = await decode(file);assert.ok(message.timestamp > 0 && message.seq >= 0 && message.seq <= 255);
    const metrics = new Map(message.metrics.map(m => [schema.get(m.alias).name, m]));
    assert.equal(metrics.get('Batch/Id').intValue, batch);
    assert.equal(Boolean(metrics.get('Temperature').isHistorical), historical);
    assert.equal(metrics.get('Temperature').doubleValue, batch === 42 ? 26.31 : 27.5);
    assert.equal(metrics.get('Magnetic/X').doubleValue, -1.5);
    assert.match(metrics.get('Batch/MessageId').stringValue, new RegExp(`/${batch}/[0-9]+$`));
    assert.ok(metrics.get('Batch/ReceivedAt').longValue > 1000000000000);
    assert.equal(metrics.get('Waveform/Samples').isNull, true);
}
for (const file of ['raw-current', 'raw-historical']) {
    const raw = (await decode(file)).metrics.find(m => m.alias === 46);
    assert.equal(raw.bytesValue.length, 40001 * 6);
    for (let i = 0; i < raw.bytesValue.length; i++) assert.equal(raw.bytesValue[i], (i * 17) & 255);
}
const nbirth = await decode('nbirth'), ndeath = await decode('ndeath');
assert.equal(nbirth.seq, 0);
assert.equal(nbirth.metrics.find(m => m.name === 'bdSeq').datatype, 4);
assert.equal(nbirth.metrics.find(m => m.name === 'bdSeq').longValue, ndeath.metrics[0].longValue);
assert.equal(Object.hasOwn(ndeath, 'seq'), false);
assert.equal((await decode('ddeath')).metrics?.length || 0, 0);
assert.equal((await decode('rebirth-name')).metrics[0].name, 'Node Control/Rebirth');
assert.equal((await decode('rebirth-alias')).metrics[0].alias, 1);
console.log('Official Eclipse Tahu schema / independent protobufjs decode PASS: births, types, aliases, properties, timestamps, historical flags and byte-perfect XYZ waveform');
