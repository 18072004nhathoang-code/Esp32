'use strict';

// Design-only state. No Bluetooth, fetch, API, audio, persistence or timers.
const phone = document.querySelector('.phone');
const sheet = document.getElementById('design-sheet');
const sheetBody = document.getElementById('sheet-body');
const songs = ['Một chút bình yên', 'Đi qua mùa nắng', 'Thành phố sau mưa'];
let state = 'connected';
let playing = true;
let stopped = false;
let track = 0;
const labels = {connected: 'Đã kết nối · minh họa', disconnected: 'Chưa kết nối · minh họa', connecting: 'Đang kết nối · minh họa', lost: 'Mất kết nối · minh họa'};

function announce(message) { document.getElementById('announcement').textContent = message; }
function showView(view) {
  document.querySelectorAll('.screen').forEach(screen => { screen.hidden = screen.id !== view; });
  document.querySelectorAll('.tabbar button').forEach(button => {
    if (button.dataset.view === view) button.setAttribute('aria-current', 'page');
    else button.removeAttribute('aria-current');
  });
  document.getElementById('app-content').scrollTop = 0;
  history.replaceState(null, '', '#' + view);
  announce('Đang xem ' + document.getElementById(view + '-title').innerText.replace(/\s+/g, ' '));
}
function render() {
  phone.dataset.connection = state;
  const available = state === 'connected';
  document.querySelectorAll('.connection-label').forEach(label => { label.textContent = labels[state]; });
  const playback = !available ? 'Chờ kết nối · minh họa' : stopped ? 'Đã dừng · minh họa' : playing ? 'Đang phát · minh họa' : 'Tạm dừng · minh họa';
  document.querySelectorAll('.playback-label').forEach(label => { label.textContent = playback; });
  document.querySelectorAll('.track-title').forEach(label => { label.textContent = songs[track]; });
  document.querySelectorAll('[data-action="play"]').forEach(button => {
    button.disabled = !available;
    button.setAttribute('aria-label', playing && !stopped ? 'Tạm dừng bản minh họa' : 'Phát bản minh họa');
    button.querySelector('use').setAttribute('href', playing && !stopped ? '#i-pause' : '#i-play');
  });
  document.querySelectorAll('[data-action="next"],[data-action="previous"],[data-action="stop"],[data-track],#volume-preview').forEach(control => { control.disabled = !available; });
  document.querySelectorAll('[data-track]').forEach(button => {
    const selected = Number(button.dataset.track) === track;
    button.setAttribute('aria-pressed', String(selected));
    button.querySelector('use').setAttribute('href', selected ? '#i-check' : '#i-play');
  });
  const banner = document.getElementById('connection-banner');
  banner.hidden = available;
  banner.textContent = state === 'lost' ? 'Mẫu lỗi: kết nối đã mất. Điều khiển tạm khóa; mở Thiết bị để xem luồng kết nối lại.' : state === 'connecting' ? 'Mẫu đang kết nối. Chưa có dữ liệu hoặc xác nhận từ thiết bị.' : 'Mẫu chưa kết nối. Mở Thiết bị để xem luồng ghép đôi.';
  document.querySelectorAll('[data-state]').forEach(button => { button.setAttribute('aria-pressed', String(button.dataset.state === state)); });
}
function setTheme(theme) {
  phone.dataset.theme = theme;
  sheet.dataset.theme = theme;
  document.querySelectorAll('[data-theme]').forEach(button => {
    if (button.tagName === 'BUTTON') button.setAttribute('aria-pressed', String(button.dataset.theme === theme));
  });
}
function toggleText() {
  const large = phone.classList.toggle('large-text');
  document.getElementById('text-size').setAttribute('aria-pressed', String(large));
  announce(large ? 'Đã bật cỡ chữ lớn cho bản xem trước.' : 'Đã trở lại cỡ chữ tiêu chuẩn.');
}
function showSheet(kind) {
  const title = document.getElementById('sheet-title');
  sheetBody.replaceChildren();
  if (kind === 'pair') {
    title.textContent = 'Kết nối MiniOS';
    sheetBody.innerHTML = '<p>Luồng ghép đôi minh họa. Trong app thật, iOS sẽ yêu cầu mã đang hiển thị trên ESP32. Bản thiết kế không quét hay kết nối Bluetooth.</p><button class="sheet-device" id="sample-device"><strong>MiniOS của bạn</strong><small>Thiết bị mẫu · chạm để xem trạng thái đã kết nối</small></button><p>Không cần truy cập danh bạ, nhật ký cuộc gọi hoặc thay đổi Wi-Fi.</p>';
    document.getElementById('sample-device').onclick = () => { state = 'connected'; render(); sheet.close(); announce('Đã chuyển bản minh họa sang trạng thái kết nối. Không có kết nối thật.'); };
  } else if (kind === 'appearance') {
    title.textContent = 'Giao diện';
    sheetBody.innerHTML = '<p>Tùy chỉnh bản xem trước. Không lưu cấu hình lên điện thoại hoặc MiniOS.</p><div class="sheet-actions"><button data-theme="light">Chế độ sáng</button><button data-theme="dark">Chế độ tối</button><button id="sheet-text-size">Đổi cỡ chữ</button><button id="sheet-disconnected">Xem mẫu chưa kết nối</button><button id="sheet-connecting">Xem mẫu đang kết nối</button><button id="sheet-lost">Xem mẫu mất kết nối</button></div>';
    setTheme(phone.dataset.theme);
    document.getElementById('sheet-text-size').onclick = () => { toggleText(); sheet.close(); };
    for (const sample of ['disconnected', 'connecting', 'lost']) document.getElementById('sheet-' + sample).onclick = () => { state = sample; render(); sheet.close(); };
  } else {
    title.textContent = 'Một bản thiết kế. Không phải kết nối thật.';
    sheetBody.innerHTML = '<p>MiniOS cho iPhone: ba màn hình Tổng quan, Âm nhạc và Thiết bị. Chuyển tab, đổi giao diện, chọn bài và xem các trạng thái minh họa.</p><p>Tên bài, âm lượng và trạng thái chỉ là dữ liệu thiết kế. Không phát âm thanh, không gửi lệnh BLE, không gọi AI hoặc đọc dữ liệu từ ESP32.</p><p>Firmware, Wi-Fi, pinout và dữ liệu NVS được giữ nguyên.</p>';
  }
  sheet.showModal();
}
document.addEventListener('click', event => {
  const button = event.target.closest('button');
  if (!button || button.disabled) return;
  if (button.dataset.view) showView(button.dataset.view);
  if (button.dataset.theme) setTheme(button.dataset.theme);
  if (button.dataset.state) { state = button.dataset.state; render(); }
  if (button.dataset.track !== undefined) { track = Number(button.dataset.track); playing = true; stopped = false; render(); announce('Đã chọn bài minh họa ' + songs[track]); }
  const action = button.dataset.action;
  if (['pair', 'about', 'appearance'].includes(action)) showSheet(action);
  if (state !== 'connected') return;
  if (action === 'play') { playing = stopped ? true : !playing; stopped = false; }
  if (action === 'stop') { playing = false; stopped = true; }
  if (action === 'next' || action === 'previous') { track = (track + (action === 'next' ? 1 : songs.length - 1)) % songs.length; stopped = false; playing = true; }
  if (['play', 'stop', 'next', 'previous'].includes(action)) { render(); announce(document.querySelector('.player-state').textContent); }
});
document.getElementById('text-size').onclick = toggleText;
document.getElementById('close-sheet').onclick = () => sheet.close();
document.getElementById('volume-preview').oninput = event => { document.getElementById('volume-label').textContent = event.target.value + '%'; };
sheet.addEventListener('click', event => { if (event.target === sheet) { const rect = sheet.getBoundingClientRect(); if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) sheet.close(); } });
const initialView = location.hash.slice(1);
showView(['overview', 'music', 'device'].includes(initialView) ? initialView : 'overview');
window.addEventListener('hashchange', () => {
  const view = location.hash.slice(1);
  if (['overview', 'music', 'device'].includes(view)) showView(view);
});
render();
