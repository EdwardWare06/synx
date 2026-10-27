// JSON-RPC framing for a stdio language server. No VS Code dependency.
'use strict';
const { EventEmitter } = require('node:events');
const MAX_BODY = 16 * 1024 * 1024;
class Transport extends EventEmitter {
    constructor(child) {
        super();
        this.child = child;
        this.buffer = Buffer.alloc(0);
        this.length = undefined;
        this.pending = new Map();
        this.nextId = 1;
        this.closed = false;
        child.stdout.on('data', data => this.receive(data));
        child.on('error', error => this.close(error));
        child.on('exit', (code, signal) => this.close(new Error(`Language server exited (${signal || code}).`)));
        child.stdin.on('error', error => this.close(error));
    }
    send(message) {
        if (this.closed) throw new Error('Language server is closed.');
        const body = Buffer.from(JSON.stringify({ jsonrpc: '2.0', ...message }), 'utf8');
        this.child.stdin.write(Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body]));
    }
    notify(method, params) { this.send({ method, params }); }
    request(method, params, token) {
        if (token?.isCancellationRequested) return Promise.reject(new Error('Request cancelled.'));
        const id = this.nextId++;
        return new Promise((resolve, reject) => {
            const pending = { resolve, reject, cancellation: undefined, timer: undefined };
            this.pending.set(id, pending);
            pending.timer = setTimeout(() => {
                this.finish(id, undefined, new Error(`Language server request timed out: ${method}`));
                if (!this.closed) this.notify('$/cancelRequest', { id });
            }, 120_000);
            pending.cancellation = token?.onCancellationRequested(() => {
                this.finish(id, undefined, new Error('Request cancelled.'));
                if (!this.closed) this.notify('$/cancelRequest', { id });
            });
            if (!this.pending.has(id)) { pending.cancellation?.dispose(); return; }
            try { this.send({ id, method, params }); } catch (error) { this.finish(id, undefined, error); }
        });
    }
    finish(id, value, error) {
        const pending = this.pending.get(id);
        if (!pending) return;
        this.pending.delete(id);
        clearTimeout(pending.timer);
        pending.cancellation?.dispose();
        if (error) pending.reject(error); else pending.resolve(value);
    }
    receive(data) {
        if (this.closed) return;
        this.buffer = Buffer.concat([this.buffer, data]);
        try {
            for (;;) {
                if (this.length === undefined) {
                    const end = this.buffer.indexOf('\r\n\r\n');
                    if (end < 0) { if (this.buffer.length > 8192) throw new Error('Oversized LSP header.'); return; }
                    if (end > 8192) throw new Error('Oversized LSP header.');
                    const headers = this.buffer.subarray(0, end).toString('ascii').split('\r\n');
                    const sizes = headers.filter(line => /^content-length:/i.test(line));
                    if (sizes.length !== 1 || !/^content-length: *\d+$/i.test(sizes[0])) throw new Error('Invalid LSP Content-Length.');
                    this.length = Number(sizes[0].split(':')[1]);
                    if (!Number.isSafeInteger(this.length) || this.length > MAX_BODY) throw new Error('Oversized LSP body.');
                    this.buffer = this.buffer.subarray(end + 4);
                }
                if (this.buffer.length < this.length) return;
                const message = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(this.buffer.subarray(0, this.length)));
                this.buffer = this.buffer.subarray(this.length);
                this.length = undefined;
                if (message.jsonrpc !== '2.0') throw new Error('Invalid JSON-RPC response.');
                if (message.method) this.emit('notification', message.method, message.params);
                else if ('id' in message) this.finish(message.id, message.result,
                    message.error ? Object.assign(new Error(message.error.message), { code: message.error.code }) : undefined);
            }
        } catch (error) { this.close(error); this.child.kill(); }
    }
    close(error = new Error('Language server stopped.')) {
        if (this.closed) return;
        this.closed = true;
        for (const id of this.pending.keys()) this.finish(id, undefined, error);
        this.emit('closed', error);
    }
    async stop() {
        if (this.closed) return;
        const kill = setTimeout(() => this.child.kill(), 2000);
        try { await this.request('shutdown', null); this.notify('exit'); }
        catch { this.child.kill(); }
        finally { clearTimeout(kill); this.child.stdin.end(); this.close(); }
    }
}
module.exports = { Transport };
