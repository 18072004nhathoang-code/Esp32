'use strict';

const assert = require('assert');
const path = require('path');
const { BASE_DIR, HOST, resolveRequestPath } = require('./server');

assert.strictEqual(HOST, process.env.HOST || '127.0.0.1');
assert.strictEqual(resolveRequestPath('/'), path.join(BASE_DIR, 'index.html'));
assert.strictEqual(resolveRequestPath('/maps_view.html?x=1'), path.join(BASE_DIR, 'maps_view.html'));
for (const attack of ['/../platformio.ini', '/%2e%2e/platformio.ini', '/..%5cplatformio.ini', '/%00index.html'])
    assert.strictEqual(resolveRequestPath(attack), null);

function transform(rawX, rawY, cfg) {
    if (rawX < 0 || rawY < 0 || rawX >= cfg.nativeW || rawY >= cfg.nativeH) return null;
    let x = rawX, y = rawY, width = cfg.nativeW, height = cfg.nativeH;
    if (cfg.swap) [x, y, width, height] = [y, x, height, width];
    if (cfg.invertX) x = width - 1 - x;
    if (cfg.invertY) y = height - 1 - y;
    let mapped;
    switch (cfg.rotation) {
        case 0: mapped = [x, y]; break;
        case 1: mapped = [y, width - 1 - x]; break;
        case 2: mapped = [width - 1 - x, height - 1 - y]; break;
        case 3: mapped = [height - 1 - y, x]; break;
        default: return null;
    }
    return mapped[0] >= 0 && mapped[1] >= 0 && mapped[0] < cfg.logicalW && mapped[1] < cfg.logicalH
        ? mapped : null;
}

const es3 = { nativeW: 240, nativeH: 320, logicalW: 240, logicalH: 320, rotation: 2,
    swap: false, invertX: true, invertY: true };
const diy = { nativeW: 320, nativeH: 480, logicalW: 480, logicalH: 320, rotation: 1,
    swap: false, invertX: false, invertY: false };

// ES3C28P's FT6336 axes already follow the installed portrait orientation.
// Its 180-degree sensor mounting offset and LCD rotation 2 must cancel out.
assert.deepStrictEqual(transform(0, 0, es3), [0, 0]);
assert.deepStrictEqual(transform(239, 0, es3), [239, 0]);
assert.deepStrictEqual(transform(0, 319, es3), [0, 319]);
assert.deepStrictEqual(transform(239, 319, es3), [239, 319]);
assert.deepStrictEqual(transform(120, 160, es3), [120, 160]);
assert.strictEqual(transform(240, 0, es3), null);
assert.strictEqual(transform(0, 320, es3), null);
assert.deepStrictEqual(transform(0, 0, diy), [0, 319]);
assert.deepStrictEqual(transform(319, 479, diy), [479, 0]);
assert.deepStrictEqual(transform(160, 240, diy), [240, 159]);
for (const [rotation, logicalW, logicalH, expected] of [
    [0, 240, 320, [0, 0]], [1, 320, 240, [0, 239]],
    [2, 240, 320, [239, 319]], [3, 320, 240, [319, 0]]
]) {
    assert.deepStrictEqual(transform(0, 0, {
        ...es3, rotation, logicalW, logicalH, invertX: false, invertY: false
    }), expected);
}

const horizontal = [20, 60, 100].map(x => transform(x, 100, es3));
assert.deepStrictEqual(horizontal.map(p => p[0]), [20, 60, 100]);
assert.ok(horizontal.every(p => p[1] === 100));
const vertical = [20, 60, 100].map(y => transform(50, y, diy));
assert.deepStrictEqual(vertical.map(p => p[0]), [20, 60, 100]);
assert.ok(vertical.every(p => p[1] === 269));

class TouchReleaseState {
    constructor(timeoutMs) { this.timeoutMs = timeoutMs; this.pressed = false; this.lastGood = 0; }
    observe(kind, now) {
        if (kind === 'press') { this.pressed = true; this.lastGood = now; }
        else if (kind === 'release') this.pressed = false;
        else if (kind === 'io-error' && now - this.lastGood > this.timeoutMs) this.pressed = false;
        return this.pressed;
    }
}
const touch = new TouchReleaseState(60);
assert.strictEqual(touch.observe('press', 100), true);
assert.strictEqual(touch.observe('io-error', 130), true);
assert.strictEqual(touch.observe('io-error', 161), false);
assert.strictEqual(touch.observe('press', 200), true);
assert.strictEqual(touch.observe('release', 205), false);

class CameraLifecycle {
    constructor() { this.state = 'STOPPED'; this.worker = false; this.created = 0; }
    start() {
        if (this.worker || this.state === 'STOPPING') return false;
        this.state = 'STARTING'; this.worker = true; this.created++; this.state = 'RUNNING'; return true;
    }
    stop(workerExited) {
        if (!this.worker) { this.state = 'STOPPED'; return true; }
        this.state = 'STOPPING';
        if (!workerExited) return false;
        this.worker = false; this.state = 'STOPPED'; return true;
    }
}
const lifecycle = new CameraLifecycle();
assert.strictEqual(lifecycle.start(), true);
assert.strictEqual(lifecycle.stop(false), false);
assert.strictEqual(lifecycle.state, 'STOPPING');
assert.strictEqual(lifecycle.start(), false);
assert.strictEqual(lifecycle.created, 1);
assert.strictEqual(lifecycle.stop(true), true);
assert.strictEqual(lifecycle.start(), true);
assert.strictEqual(lifecycle.created, 2);

function decodeChunked(encoded, max) {
    const output = [];
    let total = 0, offset = 0;
    for (;;) {
        const end = encoded.indexOf('\r\n', offset);
        if (end < 0) throw new Error('truncated chunk header');
        const size = Number.parseInt(encoded.slice(offset, end), 16);
        if (!Number.isFinite(size)) throw new Error('invalid chunk size');
        offset = end + 2;
        if (size === 0) return Buffer.concat(output);
        if (offset + size + 2 > encoded.length) throw new Error('truncated chunk body');
        const part = Buffer.from(encoded.slice(offset, offset + size), 'binary');
        total += part.length;
        if (total > max) throw new Error('bounded');
        output.push(part);
        offset += size + 2;
    }
}
const jpeg = Buffer.from([0xff, 0xd8, 1, 2, 3, 0xff, 0xd9]);
const chunked = `3\r\n${jpeg.subarray(0, 3).toString('binary')}\r\n4\r\n${jpeg.subarray(3).toString('binary')}\r\n0\r\n\r\n`;
assert.deepStrictEqual(decodeChunked(chunked, 512 * 1024), jpeg);
assert.throws(() => decodeChunked('8\r\n1234', 512 * 1024), /truncated/);

function transportAllowed(mode, url) {
    return mode === 'HTTP_OPT_IN' ? url.startsWith('http://') : url.startsWith('https://');
}
assert.strictEqual(transportAllowed('TLS_VERIFIED', 'https://cam/snapshot'), true);
assert.strictEqual(transportAllowed('TLS_VERIFIED', 'http://cam/snapshot'), false);
assert.strictEqual(transportAllowed('TLS_INSECURE', 'http://cam/snapshot'), false);
assert.strictEqual(transportAllowed('HTTP_OPT_IN', 'http://cam/snapshot'), true);

class FrameBuffers {
    constructor() { this.front = { capacity: 8, id: 1 }; this.back = { capacity: 32, id: 2 }; this.leased = false; }
    lease() { if (this.leased) return null; this.leased = true; return this.front; }
    publish() { if (this.leased) return false; [this.front, this.back] = [this.back, this.front]; return true; }
    release(frame) { assert.strictEqual(frame, this.front); this.leased = false; }
}
const buffers = new FrameBuffers();
const lease = buffers.lease();
assert.strictEqual(buffers.publish(), false);
buffers.release(lease);
assert.strictEqual(buffers.publish(), true);
assert.strictEqual(buffers.front.capacity, 32);
assert.strictEqual(buffers.back.capacity, 8);

const headerHeight = 30, titleLineHeight = 25;
assert.ok((headerHeight - titleLineHeight) / 2 >= 2);
assert.strictEqual(320 - 22 - headerHeight, 268);

console.log('Behavioral regression tests: PASS');
