import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// Execute the actual App effect, not a separate model of the intended fix.
const source = readFileSync(new URL('../src/App.jsx', import.meta.url), 'utf8');
const start = source.indexOf('useEffect(() => {', source.indexOf('/* WebSocket */'));
const suffix = '}, [applyChannelOperationResult, refreshDeviceDetails, recordHomeActivity, loadLatestVibration]);';
const end = source.indexOf(suffix, start);
assert.ok(start >= 0 && end > start);
const effect = source.slice(start, end + suffix.length);

function harness() {
    const sockets = [], closed = [], timers = new Map(), activities = [];
    let cleanup, timerId = 0, updates = 0;
    class Socket {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        constructor() { this.readyState = Socket.CONNECTING; sockets.push(this); }
        close() {
            this.readyState = Socket.CLOSING;
            // A callback already queued by the browser may survive detachment.
            const callback = this.onclose;
            closed.push(() => { this.readyState = Socket.CLOSED; callback?.({}); });
        }
        open() { this.readyState = Socket.OPEN; this.onopen?.({}); }
    }
    const noop = () => {};
    const context = {
        WebSocket: Socket, window: { location: { hostname: 'test' } },
        console: { log: noop, error: noop },
        setTimeout(fn, delay) { assert.equal(delay, 3000); timers.set(++timerId, fn); return timerId; },
        clearTimeout(id) { timers.delete(id); },
        useEffect(fn) { cleanup = fn(); },
        applyChannelOperationResult: noop, refreshDeviceDetails: noop, loadLatestVibration: noop,
        recordHomeActivity(event) { activities.push(event); },
        setJoinState() { updates++; }, setGatewayStatus() { updates++; }, setWhitelist() { updates++; }
    };
    return {
        sockets, activities, timers,
        mount() { vm.runInNewContext(effect, context); return cleanup; },
        flushClose() { closed.splice(0).forEach(fn => fn()); },
        tick() { const pending = [...timers.values()]; timers.clear(); pending.forEach(fn => fn()); },
        active() { return sockets.filter(s => [Socket.CONNECTING, Socket.OPEN].includes(s.readyState)); },
        broadcast() { for (const s of sockets.filter(s => s.readyState === Socket.OPEN)) s.onmessage?.({ data: JSON.stringify({ type: 'name_update', mac: 'test', name: 'sensor' }) }); },
        updates() { return updates; }
    };
}

// Development StrictMode: setup -> cleanup -> setup, then async close events.
const strict = harness();
const firstCleanup = strict.mount();
const lateMessage = strict.sockets[0].onmessage;
const lateClose = strict.sockets[0].onclose;
const lateError = strict.sockets[0].onerror;
firstCleanup();
const finalCleanup = strict.mount();
strict.flushClose(); lateClose(); lateError({});
lateMessage({ data: JSON.stringify({ type: 'name_update', mac: 'test' }) });
strict.tick();
assert.equal(strict.sockets.length, 2);
assert.equal(strict.active().length, 1);
assert.equal(strict.activities.length, 0);
strict.active()[0].open(); strict.broadcast();
assert.equal(strict.activities.length, 1); // One event, one activity; no text dedupe.
finalCleanup(); const finalUpdates = strict.updates();
strict.flushClose(); strict.tick();
assert.equal(strict.active().length, 0);
assert.equal(strict.updates(), finalUpdates);

// Real disconnects still reconnect once after three seconds, without growth.
const reconnect = harness();
const stopReconnect = reconnect.mount();
for (let i = 0; i < 4; i++) {
    const socket = reconnect.active()[0]; socket.open();
    const staleClose = socket.onclose;
    socket.close(); reconnect.flushClose(); staleClose();
    assert.equal(reconnect.timers.size, 1);
    reconnect.tick(); staleClose();
    assert.equal(reconnect.timers.size, 0);
    assert.equal(reconnect.active().length, 1);
}
reconnect.active()[0].open(); reconnect.broadcast();
assert.equal(reconnect.activities.length, 1);
stopReconnect(); reconnect.flushClose(); reconnect.tick();
assert.equal(reconnect.active().length, 0);

// Unmount before the reconnect timer fires: no ghost connection.
const pending = harness();
const stopPending = pending.mount();
pending.active()[0].close(); pending.flushClose();
const queuedTimer = [...pending.timers.values()][0];
assert.equal(pending.timers.size, 1);
stopPending(); queuedTimer(); pending.tick();
assert.equal(pending.sockets.length, 1);
assert.equal(pending.active().length, 0);
console.log('Actual App WebSocket effect: StrictMode single connection/event, repeated reconnects, stale callbacks and unmount PASS');
