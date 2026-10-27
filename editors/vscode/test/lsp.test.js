// Protocol tests use an in-memory child process and arbitrary frame boundaries.
'use strict';
const { PassThrough } = require('node:stream');
const { EventEmitter } = require('node:events');
const assert = require('node:assert/strict');
const test = require('node:test');
const { Transport } = require('../src/lsp-transport');
function setup() {
    const child = new EventEmitter();
    child.stdout = new PassThrough(); child.stdin = new PassThrough(); child.kill = () => child.emit('exit', 1);
    return { child, transport: new Transport(child) };
}
function frame(message) { const body = Buffer.from(JSON.stringify(message)); return Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body]); }
test('fragmented UTF-8 and back-to-back frames preserve IDs and notifications', async () => {
    const { child, transport } = setup();
    const notices = []; transport.on('notification', (...n) => notices.push(n));
    const first = transport.request('hover', {}), second = transport.request('references', {});
    const data = Buffer.concat([frame({ jsonrpc: '2.0', method: 'notice', params: '\u{1f600}' }), frame({ jsonrpc: '2.0', id: 2, result: [7] }), frame({ jsonrpc: '2.0', id: 1, result: 'caf\u00e9' })]);
    for (const byte of data) child.stdout.write(Buffer.from([byte]));
    assert.equal(await first, 'caf\u00e9'); assert.deepEqual(await second, [7]); assert.deepEqual(notices, [['notice', '\u{1f600}']]); transport.close();
});
test('protocol errors reject pending requests', async () => {
    const { child, transport } = setup();
    const pending = transport.request('rename', {});
    child.stdout.write(frame({ jsonrpc: '2.0', id: 1, error: { code: -32602, message: 'conflict' } }));
    await assert.rejects(pending, { code: -32602, message: 'conflict' }); transport.close();
});
test('malformed and oversized headers stop the child and reject requests', async () => {
    for (const header of ['Content-Length: 3\r\nContent-Length: 3\r\n\r\n{} ', 'Content-Length: 16777217\r\n\r\n', 'Content-Length: -1\r\n\r\n']) {
        const { child, transport } = setup(); const pending = transport.request('hover', {});
        child.stdout.write(header); await assert.rejects(pending); assert.equal(transport.closed, true);
    }
});
test('cancellation rejects once and late results are ignored', async () => {
    const { child, transport } = setup(); let cancel;
    const token = { isCancellationRequested: false, onCancellationRequested(fn) { cancel = fn; return { dispose() {} }; } };
    const pending = transport.request('hover', {}, token); cancel(); await assert.rejects(pending, /cancelled/);
    child.stdout.write(frame({ jsonrpc: '2.0', id: 1, result: 4 })); assert.equal(transport.pending.size, 0); transport.close();
});
test('process exit clears every pending request', async () => {
    const { child, transport } = setup(); const pending = transport.request('hover', {}); child.emit('exit', 1);
    await assert.rejects(pending, /exited/); assert.equal(transport.pending.size, 0);
});

test('cancellation during subscription leaves no pending request', async () => {
    const { transport } = setup();
    const token = { isCancellationRequested: false, onCancellationRequested(fn) { fn(); return { dispose() {} }; } };
    await assert.rejects(transport.request('hover', {}, token), /cancelled/);
    assert.equal(transport.pending.size, 0); transport.close();
});
