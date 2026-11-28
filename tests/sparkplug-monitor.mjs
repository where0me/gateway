import assert from 'node:assert/strict';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { readFile, copyFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';
const dir = resolve(process.argv[2] || 'out/sparkplug-tests');
const require = createRequire(resolve(dir, 'decoder/package.json'));
const packet = require('mqtt-packet'), protobuf = require('protobufjs');
const root = await protobuf.load(resolve(dir, 'sparkplug_b.proto'));
const Payload = root.lookupType('org.eclipse.tahu.protobuf.Payload');
await copyFile(resolve('integrations/sparkplug/monitor.mjs'), resolve(dir, 'decoder/monitor.mjs'));
await copyFile(resolve('integrations/sparkplug/sparkplug_b.proto'), resolve(dir, 'decoder/sparkplug_b.proto'));
const fixtures = await Promise.all(['nbirth', 'dbirth', 'collection-current', 'raw-current', 'ddeath'].map(name => readFile(resolve(dir, `${name}.pb`))));
let gotCommand = false;
const sockets = new Set();
const server = net.createServer(socket => {
    sockets.add(socket);socket.on('close', () => sockets.delete(socket));
    const parser = packet.parser({ protocolVersion: 4 });socket.on('data', bytes => parser.parse(bytes));
    parser.on('packet', message => {
        if (message.cmd === 'connect') {
            assert.equal(message.username, 'test-user');assert.equal(message.password.toString(), 'test-secret-only');
            socket.write(packet.generate({ cmd: 'connack', sessionPresent: false, returnCode: 0 }));
        } else if (message.cmd === 'subscribe') {
            assert.equal(message.subscriptions[0].topic, 'spBv1.0/test-group/+/test-gw/#');
            socket.write(packet.generate({ cmd: 'suback', messageId: message.messageId, granted: [0] }));
        } else if (message.cmd === 'publish') {
            assert.equal(message.topic, 'spBv1.0/test-group/NCMD/test-gw');assert.equal(message.retain, false);
            const command = Payload.decode(message.payload);assert.equal(command.metrics[0].name, 'Node Control/Rebirth');assert.equal(command.metrics[0].booleanValue, true);
            gotCommand = true;socket.write(packet.generate({ cmd: 'puback', messageId: message.messageId }));
            const types = ['NBIRTH', 'DBIRTH', 'DDATA', 'DDATA', 'DDEATH'];
            fixtures.forEach((payload, i) => socket.write(packet.generate({ cmd: 'publish', qos: 0, retain: false,
                topic: `spBv1.0/test-group/${types[i]}/test-gw${i ? '/0102030405060708' : ''}`, payload })));
        } else if (message.cmd === 'pingreq') socket.write(packet.generate({ cmd: 'pingresp' }));
    });
});
await new Promise(resolveListen => server.listen(0, '127.0.0.1', resolveListen));
const child = spawn(process.execPath, [resolve(dir, 'decoder/monitor.mjs'), `mqtt://127.0.0.1:${server.address().port}`, 'test-group', 'test-gw'], {
    env: { ...process.env, MQTT_USERNAME: 'test-user', MQTT_PASSWORD: 'test-secret-only' }, stdio: ['ignore', 'pipe', 'pipe']
});
let output = '', errors = '';
child.stdout.on('data', bytes => { output += bytes; });child.stderr.on('data', bytes => { errors += bytes; });
try {
    await new Promise((done, reject) => {
        const timeout = setTimeout(() => reject(new Error(`Monitor timeout: ${errors}`)), 8000);
        const check = () => { if (output.includes('/DDEATH/')) { clearTimeout(timeout);child.stdout.off('data', check);done(); } };
        child.stdout.on('data', check);child.once('error', reject);
    });
    assert.ok(gotCommand);assert.equal(errors, '');assert.ok(!output.includes('test-secret-only'));
    const messages = output.split(/\r?\n/).filter(line => line.startsWith('{')).map(line => JSON.parse(line));
    const data = messages.filter(m => m.topic.includes('/DDATA/'));
    assert.equal(data.length, 2);
    assert.equal(data[0].metrics.find(m => m.name === 'Temperature').value, 27.5);
    assert.equal(data[1].metrics.find(m => m.name === 'Waveform/Samples').value.bytes, 240006);
    console.log('Sparkplug monitor PASS: actual MQTT.js TCP authentication/subscription, Rebirth request, official protobuf decoding and alias-resolved readable scalar/raw output');
} finally {
    child.kill();for (const socket of sockets) socket.destroy();await new Promise(done => server.close(done));
}
