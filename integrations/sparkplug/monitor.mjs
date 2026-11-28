import mqtt from 'mqtt';
import protobuf from 'protobufjs';
import { fileURLToPath } from 'node:url';

// Diagnostic subscriber, not a historian or a certified Sparkplug Host.
// Credentials are read from the environment and never written to the console.
const [address = 'mqtt://127.0.0.1:1883', group = 'factory01', gateway = 'gw001'] = process.argv.slice(2);
if (![group, gateway].every(v => /^[A-Za-z0-9_.-]{1,63}$/.test(v))) throw new Error('Invalid group/gateway ID');
const endpoint = new URL(address);
if (endpoint.protocol !== 'mqtt:' || endpoint.username || endpoint.password) throw new Error('Use mqtt://host:port and MQTT_USERNAME / MQTT_PASSWORD');
const root = await protobuf.load(fileURLToPath(new URL('./sparkplug_b.proto', import.meta.url)));
const Payload = root.lookupType('org.eclipse.tahu.protobuf.Payload');
const names = new Map(), lastRequest = new Map();
const client = mqtt.connect(address, {
    protocolVersion: 4, clean: true, clientId: `spb-monitor-${process.pid}-${Date.now()}`,
    username: process.env.MQTT_USERNAME, password: process.env.MQTT_PASSWORD
});
function rebirth(node) {
    if (Date.now() - (lastRequest.get(node) || 0) < 1000) return;
    lastRequest.set(node, Date.now());
    const payload = Payload.encode(Payload.create({ timestamp: Date.now(), metrics: [
        { name: 'Node Control/Rebirth', datatype: 11, booleanValue: true }
    ] })).finish();
    client.publish(`spBv1.0/${group}/NCMD/${node}`, payload, { qos: 1, retain: false });
}
client.on('connect', () => {
    console.log(`Connected to ${endpoint.hostname}:${endpoint.port || 1883}; group=${group}, gateway=${gateway}`);
    client.subscribe(`spBv1.0/${group}/+/${gateway}/#`, { qos: 0 }, (error, granted) => {
        if (error || !granted?.length || granted.some(g => g.qos === 128)) {
            console.error('Subscription denied'); client.end(); return;
        }
        names.clear(); rebirth(gateway);
    });
});
client.on('message', (topic, bytes) => {
    const [, , type, node, device] = topic.split('/');
    if (!['NBIRTH', 'DBIRTH', 'NDATA', 'DDATA', 'NDEATH', 'DDEATH'].includes(type)) return;
    try {
        const payload = Payload.toObject(Payload.decode(bytes), { longs: Number, bytes: Buffer });
        const key = `${node}/${device || ''}`;
        if (type === 'NBIRTH') for (const known of names.keys()) if (known.startsWith(`${node}/`)) names.delete(known);
        if (type === 'NBIRTH' || type === 'DBIRTH') names.set(key, new Map());
        const aliases = names.get(key);
        const metrics = (payload.metrics || []).map(metric => {
            if (metric.name && metric.alias != null) aliases?.set(metric.alias, metric.name);
            const name = metric.name || aliases?.get(metric.alias) || `unknown-alias-${metric.alias}`;
            if (name.startsWith('unknown-alias-')) rebirth(node);
            let value = null;
            if (!metric.isNull) for (const field of ['intValue', 'longValue', 'floatValue', 'doubleValue', 'booleanValue', 'stringValue', 'bytesValue']) {
                if (Object.hasOwn(metric, field)) { value = metric[field]; break; }
            }
            if (Buffer.isBuffer(value)) value = { bytes: value.length, encoding: 'xyz-int16-le' };
            return { name, value, ...(metric.isHistorical ? { historical: true } : {}), timestamp: metric.timestamp };
        });
        console.log(JSON.stringify({ topic, seq: payload.seq, timestamp: payload.timestamp, metrics }));
    } catch { console.error(`Invalid Sparkplug payload on ${topic}`); }
});
client.on('error', () => console.error('MQTT connection/authentication error; check endpoint and credentials'));
process.on('SIGINT', () => client.end());
