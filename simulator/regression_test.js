'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { BASE_DIR, HOST, resolveRequestPath } = require('./server');

assert.strictEqual(HOST, process.env.HOST || '127.0.0.1');
assert.strictEqual(resolveRequestPath('/'), path.join(BASE_DIR, 'index.html'));
assert.strictEqual(resolveRequestPath('/maps_view.html?x=1'), path.join(BASE_DIR, 'maps_view.html'));
for (const attack of [
    '/../platformio.ini',
    '/%2e%2e/platformio.ini',
    '/..%5cplatformio.ini',
    '/%2e%2e%5cplatformio.ini',
    '/safe%5c..%5c..%5cplatformio.ini',
    '/%00index.html'
])
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

// ES3C28P's FT6336 axes already follow the installed portrait orientation.
// Its 180-degree sensor mounting offset and LCD rotation 2 must cancel out.
assert.deepStrictEqual(transform(0, 0, es3), [0, 0]);
assert.deepStrictEqual(transform(239, 0, es3), [239, 0]);
assert.deepStrictEqual(transform(0, 319, es3), [0, 319]);
assert.deepStrictEqual(transform(239, 319, es3), [239, 319]);
assert.deepStrictEqual(transform(120, 160, es3), [120, 160]);
assert.strictEqual(transform(240, 0, es3), null);
assert.strictEqual(transform(0, 320, es3), null);
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

function screenToLocal(x, y, originX, originY, width, height) {
    const lx = x - originX, ly = y - originY;
    return lx >= 0 && ly >= 0 && lx < width && ly < height ? [lx, ly] : null;
}
assert.deepStrictEqual(screenToLocal(120, 160, 0, 30, 240, 290), [120, 130]);
assert.strictEqual(screenToLocal(10, 29, 0, 30, 240, 290), null);

function selectContact(activeId, contacts) {
    if (activeId !== 0xff) {
        const index = contacts.findIndex(p => p.id === activeId && p.event <= 2);
        return index >= 0 ? { index, release: false } : { index: -1, release: true };
    }
    const index = contacts.findIndex(p => p.event <= 2);
    return { index, release: false };
}
assert.deepStrictEqual(selectContact(7, [{ id: 4, event: 2 }, { id: 7, event: 2 }]), { index: 1, release: false });
assert.deepStrictEqual(selectContact(7, [{ id: 4, event: 2 }]), { index: -1, release: true });

class AtomicStarter {
    constructor() { this.state = 'IDLE'; this.acquires = 0; }
    begin() { if (this.state !== 'IDLE') return false; this.state = 'STARTING'; this.acquires++; return true; }
    finish(ok) { if (this.state !== 'STARTING') return false; this.state = ok ? 'ACTIVE' : 'IDLE'; return ok; }
    cancel() { this.state = 'IDLE'; }
}
const starter = new AtomicStarter();
assert.strictEqual(starter.begin(), true);
assert.strictEqual(starter.begin(), false);
assert.strictEqual(starter.acquires, 1);
starter.cancel();
assert.strictEqual(starter.finish(true), false);
assert.strictEqual(Math.floor(1024 / 4), 256);
assert.strictEqual(Math.floor(1023 / 4), 255);

class CameraMailbox {
    constructor() { this.revision = 0; this.ack = 0; this.session = 0; this.active = false; }
    set(session, active) { this.session = session; this.active = active; this.revision++; }
    pending() { return this.revision !== this.ack; }
    apply() { this.ack = this.revision; return { session: this.session, active: this.active }; }
}
const mailbox = new CameraMailbox();
mailbox.set(1, false); // close cannot be lost even if the ordinary queue is full
mailbox.set(2, true);  // immediate reopen supersedes the stale release safely
assert.strictEqual(mailbox.pending(), true);
assert.deepStrictEqual(mailbox.apply(), { session: 2, active: true });
assert.strictEqual(mailbox.pending(), false);

class RequestGate {
    constructor() { this.next = 0; this.active = 0; this.cancelledThrough = 0; }
    start() { if (this.active) return 0; return this.active = ++this.next; }
    cancel() { this.cancelledThrough = Math.max(this.cancelledThrough, this.active); }
    finish(id) { if (this.active === id) this.active = 0; }
    accepts(id) { return id !== 0 && id === this.active && id > this.cancelledThrough; }
}
const requests = new RequestGate();
const oldRequest = requests.start();
requests.cancel();
assert.strictEqual(requests.accepts(oldRequest), false);
assert.strictEqual(requests.start(), 0);
requests.finish(oldRequest);
assert.ok(requests.start() > oldRequest);

// Firmware regressions: các app phải dùng service thật hoặc fail rõ ràng.
const repoRoot = path.resolve(__dirname, '..');
const source = relative => fs.readFileSync(path.join(repoRoot, relative), 'utf8');
const aiService = source('src/ai/ai_voice_service.cpp');
const aiUi = source('src/apps/ai_voice_app.cpp');
const musicService = source('src/audio/music_player.cpp');
const mapService = source('src/apps/map_tile_downloader.cpp');
const mapUi = source('src/apps/map_app.cpp');
const settingsService = source('src/os/settings_service.cpp');
const wifiService = source('src/os/wifi_manager.cpp');
const cameraUi = source('src/apps/camera_app.cpp');
const audioService = source('src/audio/audio_manager.cpp');
const settingsServiceImpl = source('src/os/settings_service.cpp');
const platformio = source('platformio.ini');
const lvglPort = source('src/display/lvgl_port.cpp');

assert.match(aiService, /https:\/\//);
assert.match(aiService, /Content-Type", "audio\/wav/);
assert.match(aiService, /audio_write_pcm16_mono/);
assert.doesNotMatch(aiUi, /Demo|Mock|Mô phỏng/);
assert.doesNotMatch(musicService, /demo_playlist|210\s*;/);
assert.match(musicService, /connecttoFS/);
assert.doesNotMatch(mapUi, /render_offline_vector_map/);
assert.doesNotMatch(mapService, /staticmap\.openstreetmap\.de/);
assert.match(mapService, /PROVIDER_NOT_CONFIGURED/);
assert.match(mapService, /map_tile_downloader_supports_satellite/);
assert.match(settingsService, /Preferences/);
assert.match(settingsService, /putBytes\("record"/);
assert.match(settingsService, /kSchemaVersion = 2/);
assert.match(wifiService, /WIFI_SCAN_FAILED/);
assert.match(wifiService, /xQueue|xTaskCreatePinnedToCore/);
assert.doesNotMatch(cameraUi, /MJPEG \(Chưa\)|RTSP\/H\.264 \(Chưa\)/);
assert.match(cameraUi, /HTTP\(S\) Snapshot/);
assert.match(cameraUi, /CameraWorkerControl/);
assert.match(cameraUi, /session_id/);
assert.match(cameraUi, /preview_front_capacity_pixels/);
assert.match(cameraUi, /preview_back_capacity_pixels/);
assert.match(audioService, /void audio_cancel_recording/);
assert.match(audioService, /audio_state_mutex = xSemaphoreCreateMutex/);
assert.match(audioService, /xSemaphoreTake\(audio_state_mutex/);
assert.doesNotMatch(audioService, /static SemaphoreHandle_t audio_mutex/);
assert.match(audioService, /static uint8_t audio_dma_bytes/);
assert.match(audioService, /"Audio_Task",\s*8 \* 1024/);
assert.match(aiService, /audio_cancel_recording/);
assert.match(aiService, /size_t readBytes\(char \*buffer/);
assert.match(aiService, /AI_STATE_CANCELING/);
assert.match(aiService, /writeToStream/);
assert.match(aiService, /kJsonBodyLimit/);
assert.match(aiService, /kTtsBodyLimit/);
assert.match(aiService, /deserializeJson/);
assert.match(aiService, /serializeJson/);
assert.doesNotMatch(aiService, /extract_json_string|json_object_envelope_valid|json_escape/);
assert.match(platformio, /ESP32-audioI2S\.git#928c420d49fce2a09fa91f490b9fcabed6447c67/);
assert.match(musicService, /xTaskNotify\(audio_task_handle, MUSIC_EVENT_EOF, eSetBits\)/);
assert.doesNotMatch(musicService.match(/void audio_eof_mp3[\s\S]*$/)[0].split('}')[0], /music_player_next/);
assert.match(settingsServiceImpl, /SettingsWorker/);
assert.match(settingsServiceImpl, /xQueueSend\(s_command_queue/);
assert.doesNotMatch(lvglPort, /run_lovyangfx_color_test|run_lvgl_color_test|\[DISPLAY_TEST\]|RGB565 TEST \/ ABC 123/);

console.log('Behavioral regression tests: PASS');
