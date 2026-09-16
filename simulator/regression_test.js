'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { BASE_DIR, HOST, resolveRequestPath } = require('./server');

assert.strictEqual(HOST, process.env.HOST || '127.0.0.1');
assert.strictEqual(resolveRequestPath('/'), path.join(BASE_DIR, 'index.html'));
assert.strictEqual(resolveRequestPath('/maps_view.html?x=1'), path.join(BASE_DIR, 'maps_view.html'));
assert.strictEqual(resolveRequestPath('/../platformio.ini'), null);
assert.strictEqual(resolveRequestPath('/%2e%2e/platformio.ini'), null);
assert.strictEqual(resolveRequestPath('/..%5cplatformio.ini'), null);
assert.strictEqual(resolveRequestPath('/%00index.html'), null);

const root = path.resolve(BASE_DIR, '..');
const read = relative => fs.readFileSync(path.join(root, relative), 'utf8');
const music = read('src/audio/music_player.cpp');
const map = read('src/apps/map_tile_downloader.cpp');
const camera = read('src/apps/camera_app.cpp');
const networkCamera = read('src/camera/network_camera_service.cpp');
const ui = read('src/ui/ui_manager.cpp');
const display = read('src/display/lvgl_port.cpp');
const touch = read('src/shared_i2c_bus.cpp');
const touchUi = read('src/ui/touch_test.cpp');
const main = read('src/main.cpp');
const es3c28p = read('include/boards/board_es3c28p.hpp');

assert.match(music, /xQueueSend\([^\n]+\) != pdTRUE/);
assert.match(music, /if \(music_owns_audio\) return true;/);
assert.match(map, /count_pixels < \(copy_rows \* \(size_t\)MAP_CANVAS_WIDTH\)/);
assert.match(map, /tjpg_guard_lock\(\)/);
assert.match(camera, /tjpg_guard_lock\(\)/);
assert.match(networkCamera, /https:\/\//);
assert.match(networkCamera, /HTTP plaintext; tài khoản và mật khẩu camera có thể bị nghe lén/);
assert.ok(ui.indexOf('invalidate_active_app_widgets();') < ui.indexOf('lv_obj_clean(app_content_container);'));
assert.match(display, /static_assert\(LV_COLOR_DEPTH == 16/);
assert.match(display, /static_assert\(LV_COLOR_16_SWAP == 0/);
assert.match(display, /const lgfx::rgb565_t \*pixels/);
assert.match(display, /writePixelsDMA\(pixels, pixel_count\)/);
assert.doesNotMatch(display, /getBool\("swap"/);
assert.match(display, /run_lovyangfx_color_test\(\)/);
assert.match(display, /run_lvgl_color_test\(display\)/);
assert.match(touch, /touches != 1/);
assert.match(touch, /shared_i2c_touch_map_raw/);
assert.match(touch, /validation_rms_error > 8\.0f/);
assert.ok(touch.indexOf('prefs.putBytes("data"') > touch.indexOf('validation_rms_error > 8.0f'));
assert.match(touchUi, /CalibrationPhase::Validate/);
assert.match(touchUi, /sequence != last_sample_sequence/);
assert.match(touchUi, /if \(!wait_for_release && sample_count > 0\) sample_count = 0/);
assert.match(ui, /ui_touch_test_is_calibration_blocking\(\)/);
assert.ok(main.indexOf('ui_init();') < main.indexOf('wifi_manager_init();'));
assert.match(main, /digitalRead\(BOARD_BOOT_PIN\) == LOW/);
assert.match(es3c28p, /BOARD_LCD_WIDTH\s+240/);
assert.match(es3c28p, /BOARD_LCD_HEIGHT\s+320/);
assert.match(es3c28p, /BOARD_LCD_ROTATION\s+2/);

console.log('Regression checks: PASS');
