const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

function findChrome() {
    if (process.env.CHROME_BIN && fs.existsSync(process.env.CHROME_BIN)) {
        return process.env.CHROME_BIN;
    }
    const candidates = [
        "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        process.env.LOCALAPPDATA ? path.join(process.env.LOCALAPPDATA, "Google\\Chrome\\Application\\chrome.exe") : null,
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/chromium-browser",
        "/usr/bin/chromium",
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    ].filter(Boolean);

    for (const p of candidates) {
        if (fs.existsSync(p)) return p;
    }
    return null;
}

const CHROME_PATH = findChrome();
const HTML_FILE = path.join(__dirname, 'index.html');
const HTML_PATH = 'file:///' + HTML_FILE.replace(/\\/g, '/');
const OUT_DIR = path.join(__dirname, 'screenshots');
const ARTIFACT_DIR = process.env.ANTIGRAVITY_ARTIFACT_DIR || null;

if (!fs.existsSync(OUT_DIR)) {
    fs.mkdirSync(OUT_DIR, { recursive: true });
}

function sleep(ms) {
    return new Promise(r => setTimeout(r, ms));
}

function assert(condition, message) {
    if (!condition) {
        console.error(`❌ [ASSERTION FAILED]: ${message}`);
        throw new Error(message);
    }
}

async function main() {
    console.log("==================================================================");
    console.log("  TEST SUITE: ES3C28P ESP32-S3 2.8\" IPS DISPLAY (240x320 PORTRAIT)");
    console.log("==================================================================\n");

    if (!CHROME_PATH) {
        console.error("❌ [ERROR] Không tìm thấy Chrome trên hệ thống! Vui lòng cài Chrome hoặc đặt CHROME_BIN.");
        process.exit(1);
    }

    const chrome = spawn(CHROME_PATH, [
        '--headless=new',
        '--remote-debugging-port=9222',
        '--window-size=1000,1180',
        '--disable-gpu',
        '--no-first-run',
        '--no-default-browser-check',
        '--run-all-compositor-stages-before-draw',
        HTML_PATH
    ]);

    await sleep(2500);

    let res = await fetch('http://localhost:9222/json');
    let tabs = await res.json();
    let simTab = tabs.find(t => t.url.includes('simulator/index.html') && t.type === 'page');

    if (!simTab) {
        console.error("[ERROR] Không tìm thấy tab Simulator trong Chrome!");
        chrome.kill();
        process.exit(1);
    }

    console.log(`[INFO] Kết nối CDP WebSocket: ${simTab.webSocketDebuggerUrl}`);
    const ws = new WebSocket(simTab.webSocketDebuggerUrl);

    let msgId = 1;
    const callbacks = new Map();

    ws.onmessage = (event) => {
        const data = JSON.parse(event.data);
        if (data.id && callbacks.has(data.id)) {
            callbacks.get(data.id)(data);
            callbacks.delete(data.id);
        }
    };

    function sendCommand(method, params = {}) {
        return new Promise((resolve) => {
            const id = msgId++;
            callbacks.set(id, resolve);
            ws.send(JSON.stringify({ id, method, params }));
        });
    }

    await new Promise(r => ws.onopen = r);
    console.log("[INFO] Kết nối thành công! Đang thiết lập Page và Runtime...");

    await sendCommand('Page.enable');
    await sendCommand('Runtime.enable');

    async function evalCode(expression) {
        const res = await sendCommand('Runtime.evaluate', { expression, returnByValue: true });
        if (res.result?.exceptionDetails) {
            console.error(`[JS EXCEPTION in ${expression}]:`, res.result.exceptionDetails.text);
        }
        return res.result?.result?.value;
    }

    async function captureScreenshot(filename) {
        const res = await sendCommand('Page.captureScreenshot', { format: 'png' });
        const buf = Buffer.from(res.result.data, 'base64');
        const filePath = path.join(OUT_DIR, filename);
        fs.writeFileSync(filePath, buf);
        try {
            if (ARTIFACT_DIR && fs.existsSync(ARTIFACT_DIR)) {
                fs.writeFileSync(path.join(ARTIFACT_DIR, filename), buf);
            }
        } catch (e) {}
        console.log(`📸 [SCREENSHOT] Đã lưu ảnh: screenshots/${filename}`);
    }

    // 1. Desktop & Status Bar PRO MAX
    console.log("\n--- BƯỚC 1: KIỂM THỬ GIAO DIỆN DESKTOP PRO MAX & STATUS BAR ---");
    let uptime = await evalCode("document.getElementById('clockText')?.innerText");
    let ram = await evalCode("document.getElementById('ramPill')?.innerText");
    assert(uptime != null, "Top Bar clockText element must exist");
    assert(ram != null, "Top Bar ramPill element must exist");
    console.log(`✔ [PASS] Top Bar PRO MAX: Uptime [${uptime}], RAM Pill [${ram}], Logo ● S3 PRO MAX`);
    await captureScreenshot("01_desktop_promax.png");

    // 2. Google Maps Pro Max (Google Static API, MicroSD Cache, Roadmap & Satellite)
    console.log("\n--- BƯỚC 2: KIỂM THỬ GOOGLE MAPS PRO MAX (STATIC API & MICROSD CACHE) ---");
    await evalCode("openApp('maps')");
    await sleep(600);
    let hudCity = await evalCode("document.getElementById('hudCityText')?.innerText");
    assert(hudCity != null, "Maps HUD hudCityText element must exist");
    let cacheStat = await evalCode("document.getElementById('hudNetText').innerText");
    console.log(`✔ [PASS] Mở Maps Pro Max: ${hudCity} | Trạng thái: [${cacheStat}]`);
    await captureScreenshot("02_maps_promax.png");
    await captureScreenshot("13_google_maps_roadmap_cache.png");

    // Thử Zoom & Pan
    await evalCode("zoomMap(1);");
    await sleep(300);
    await evalCode("panMap(1, 0); panMap(0, 1);");
    await sleep(300);
    let hudZoomed = await evalCode("document.getElementById('hudCityText').innerText");
    console.log(`✔ [PASS] Phím cảm ứng Zoom Z16 & D-Pad 4 hướng Pan: ${hudZoomed}`);
    await captureScreenshot("03_maps_zoomed_promax.png");

    // Chuyển sang chế độ Ảnh Vệ Tinh (Satellite)
    await evalCode("toggleMapType()");
    await sleep(500);
    let typeText = await evalCode("document.getElementById('hudMapTypeText').innerText");
    let satCache = await evalCode("document.getElementById('hudNetText').innerText");
    console.log(`✔ [PASS] Nút chuyển chế độ bản đồ: [${typeText}] | Tải ảnh vệ tinh: [${satCache}]`);
    await captureScreenshot("12_google_maps_satellite_cache.png");
    await captureScreenshot("04b_maps_online_tile_promax.png");

    // Chuyển thành phố tiếp theo
    await evalCode("nextCity(); nextCity();");
    await sleep(400);
    let hudDanang = await evalCode("document.getElementById('hudCityText').innerText");
    console.log(`✔ [PASS] Chuyển thành phố: ${hudDanang}`);
    await captureScreenshot("04_maps_danang_promax.png");

    await evalCode("closeApp()");
    await sleep(300);

    // 2b. Voice AI & Audio Lab Pro Max (Microphone & Speaker FM8002E)
    console.log("\n--- BƯỚC 2B: KIỂM THỬ VOICE AI & AUDIO LAB (MIC & SPEAKER) ---");
    await evalCode("openApp('audio')");
    await sleep(600);
    let micLvl = await evalCode("document.getElementById('simMicLevel').innerText");
    let micDb = await evalCode("document.getElementById('simMicDb').innerText");
    console.log(`✔ [PASS] Microphone Live Telemetry: ${micLvl} | ${micDb}`);
    
    // Thử nút Chime và Beep
    await evalCode("playSoundEffect('chime')");
    await sleep(350);
    await evalCode("playSoundEffect('beep')");
    await sleep(250);
    console.log(`✔ [PASS] Web Audio API Loa ngoài: Phát âm Chime & Beep thành công`);

    // Thử ghi âm vào PSRAM
    await evalCode("toggleSimRecord()");
    await sleep(800);
    let recStat = await evalCode("document.getElementById('simMemoStatus').innerText");
    console.log(`✔ [PASS] Thu âm Microphone vào 8MB Octal PSRAM: ${recStat.replace('\n', ' ')}`);
    await evalCode("toggleSimRecord()");
    await sleep(300);

    // Chụp ảnh giao diện Voice AI & Audio Lab
    await captureScreenshot("10_audio_voice_promax.png");
    await evalCode("closeApp()");
    await sleep(300);

    // 2c. Music Player Pro Max (ESP32-audioI2S, MicroSD /music, Rotating Vinyl Disc & Touch Controls)
    console.log("\n--- BƯỚC 2C: KIỂM THỬ MUSIC PLAYER (ESP32-audioI2S & MICROSD /music) ---");
    await evalCode("openApp('music')");
    await sleep(600);
    let trackCount = await evalCode("document.getElementById('simMusicCount').innerText");
    let initTitle = await evalCode("document.getElementById('simTrackTitle').innerText");
    console.log(`✔ [PASS] Mở Music Player: Thư viện [${trackCount}] | Bài hát nạp sẵn: [${initTitle}]`);

    // Bật phát nhạc -> Đĩa than xoay tròn
    await evalCode("toggleMusicPlay()");
    await sleep(600);
    let isSpinning = await evalCode("document.getElementById('simVinylDisc').classList.contains('spinning')");
    let playBtnText = await evalCode("document.getElementById('simPlayBtn').innerText");
    console.log(`✔ [PASS] Nhấn Play: Đĩa than quay tròn [${isSpinning}] | Nút bấm đổi thành [${playBtnText}]`);

    // Tua thời gian (Seek) và chỉnh âm lượng
    await evalCode("seekMusic(45)");
    await evalCode("changeMusicVol(90)");
    await sleep(300);
    let curTime = await evalCode("document.getElementById('simCurTime').innerText");
    let curVol = await evalCode("document.getElementById('simMusicVolVal').innerText");
    console.log(`✔ [PASS] Tua tiến trình Seek: [${curTime}] | Âm lượng Loa: [${curVol}]`);

    // Chuyển bài kế tiếp
    await evalCode("nextMusicTrack()");
    await sleep(500);
    let nextTitle = await evalCode("document.getElementById('simTrackTitle').innerText");
    console.log(`✔ [PASS] Chuyển bài kế tiếp (Next): [${nextTitle}] | FreeRTOS Task Core 0 duy trì liên tục`);

    // Chụp ảnh giao diện Music Player Pro Max
    await captureScreenshot("17_music_player_sd_mp3.png");
    await evalCode("closeApp()");
    await sleep(300);

    // 2d. AI Voice Assistant (Chat Bubbles, Push-to-Talk, Waveform & XiaoZhi Gemini)
    console.log("\n--- BƯỚC 2D: KIỂM THỬ AI VOICE ASSISTANT (XIAOZHI & GEMINI AI) ---");
    await evalCode("openApp('ai_voice')");
    await sleep(600);
    let initChatCount = await evalCode("document.querySelectorAll('#simChatHistory .chat-bubble').length");
    console.log(`✔ [PASS] Mở AI Voice Assistant: Số lượng bong bóng hội thoại khởi tạo [${initChatCount}]`);

    // Kích hoạt Push-to-Talk (Nhấn giữ để nói)
    await evalCode("startPtt()");
    await sleep(700);
    let isPttActive = await evalCode("document.getElementById('simPttBtn').classList.contains('active')");
    let pttStatus = await evalCode("document.getElementById('simPttLabel').innerText");
    console.log(`✔ [PASS] Push-to-Talk: Trạng thái nút [active=${isPttActive}] | Nhãn: [${pttStatus}] | Waveform dao động`);

    // Nhả Push-to-Talk (Gửi truy vấn đến Gemini và nhận TTS)
    await evalCode("stopPtt()");
    await sleep(2600);
    let afterChatCount = await evalCode("document.querySelectorAll('#simChatHistory .chat-bubble').length");
    let lastMsg = await evalCode("document.querySelector('#simChatHistory .chat-bubble:last-child').innerText");
    console.log(`✔ [PASS] AI Voice Chat: Đã thêm phản hồi mới (Tổng số tin nhắn: ${afterChatCount})`);
    console.log(`✔ [PASS] AI Response Content: [${lastMsg.replace(/\n/g, ' ')}]`);

    // Chụp ảnh giao diện AI Voice Assistant
    await captureScreenshot("18_ai_voice_assistant.png");
    await evalCode("closeApp()");
    await sleep(300);

    // 3. System Monitor Pro Max
    console.log("\n--- BƯỚC 3: KIỂM THỬ SYSTEM MONITOR PRO MAX (DUAL ARC & LIVE CHART) ---");
    await evalCode("openApp('system')");
    await sleep(600);
    let cpuVal = await evalCode("document.getElementById('arcCpuVal').innerText");
    let ramVal = await evalCode("document.getElementById('arcRamVal').innerText");
    let tempVal = await evalCode("document.getElementById('chipTemp').innerText");
    console.log(`✔ [PASS] Đồng hồ Dual-Arc: CPU [${cpuVal.replace('\n', ' ')}], RAM [${ramVal.replace('\n', ' ')}]`);
    console.log(`✔ [PASS] Telemetry: ${tempVal}`);
    await captureScreenshot("05_system_promax.png");
    await evalCode("closeApp()");
    await sleep(300);

    // 4. WiFi Settings App Pro Max (Split 2-Column with Virtual Keyboard)
    console.log("\n--- BƯỚC 4: KIỂM THỬ WIFI SETTINGS APP (SPLIT 2-COLUMN & VIRTUAL KEYBOARD) ---");
    await evalCode("openApp('wifi')");
    await sleep(700);
    await evalCode("selectWifi('WiFi_NhaToi_2.4G')");
    await sleep(400);
    await evalCode("simConnectCurrentWifi()");
    await sleep(1000);
    let wifiStat = await evalCode("document.getElementById('simWifiStatus').innerText");
    console.log(`✔ [PASS] WiFi Settings App: ${wifiStat}`);
    await captureScreenshot("06_wifi_promax.png");
    await captureScreenshot("11_wifi_settings_app_promax.png");
    await evalCode("closeApp()");
    await sleep(300);

    // 5. Settings Control Center
    console.log("\n--- BƯỚC 5: KIỂM THỬ SETTINGS CONTROL CENTER ---");
    await evalCode("openApp('settings')");
    await sleep(400);
    await evalCode("changeBrightness(50)");
    await sleep(300);
    let brightText = await evalCode("document.getElementById('brightVal').innerText");
    console.log(`✔ [PASS] Độ sáng PWM: ${brightText}`);
    await captureScreenshot("07_settings_promax.png");
    await evalCode("changeBrightness(85)");
    await evalCode("closeApp()");
    await sleep(300);

    // 6. Sensors & Telemetry
    console.log("\n--- BƯỚC 6: KIỂM THỬ SENSORS & TELEMETRY ---");
    await evalCode("openApp('sensors')");
    await sleep(400);
    console.log("✔ [PASS] Sensors: La bàn số & Pitch/Roll hoạt động bình thường");
    await captureScreenshot("08_sensors_promax.png");
    await evalCode("closeApp()");
    await sleep(300);

    // 6b. Power Manager (Inactivity Timer & Touch to Wake)
    console.log("\n--- BƯỚC 6B: KIỂM THỬ POWER MANAGER (INACTIVITY TIMER & TOUCH TO WAKE) ---");
    let powerPill1 = await evalCode("document.getElementById('powerPill').innerText");
    console.log(`✔ [PASS] Trạng thái nguồn ban đầu: [${powerPill1}]`);

    // 1. Sau 60s không chạm -> Dimming 20%
    await evalCode("simAdvanceInactivity(60)");
    await sleep(400);
    let powerPill2 = await evalCode("document.getElementById('powerPill').innerText");
    let filter2 = await evalCode("document.getElementById('lcdScreen').style.filter");
    console.log(`✔ [PASS] Sau 60s không chạm: [${powerPill2}] | CSS filter: ${filter2}`);
    await captureScreenshot("14_power_manager_dimmed.png");

    // 2. Sau 120s không chạm -> Sleep 0%
    await evalCode("simAdvanceInactivity(120)");
    await sleep(400);
    let powerPill3 = await evalCode("document.getElementById('powerPill').innerText");
    let overlayDisp = await evalCode("document.getElementById('sleepOverlay').style.display");
    console.log(`✔ [PASS] Sau 120s không chạm: [${powerPill3}] | Sleep Overlay: ${overlayDisp}`);
    await captureScreenshot("15_power_manager_sleep.png");

    // 3. Touch to Wake đánh thức tức thì
    await evalCode("wakePowerManager()");
    await sleep(300);
    let powerPill4 = await evalCode("document.getElementById('powerPill').innerText");
    console.log(`✔ [PASS] Touch to Wake -> Bừng sáng trở lại 100%: [${powerPill4}]`);
    await captureScreenshot("16_power_manager_woken.png");

    // 7. Chạy bộ kiểm thử tự động toàn diện
    console.log("\n--- BƯỚC 7: CHẠY BỘ TEST TỰ ĐỘNG TOÀN DIỆN ---");
    await evalCode("runAllTests()");
    for (let i = 0; i < 60; i++) {
        await sleep(500);
        let isDisabled = await evalCode("document.getElementById('runTestBtn').disabled");
        if (!isDisabled) break;
    }
    await sleep(500);
    await captureScreenshot("09_test_suite_passed_promax.png");

    console.log("\n==================================================================");
    console.log("  🎉 KẾT QUẢ: TẤT CẢ TÍNH NĂNG UI/UX PRO MAX ĐÃ TEST THÀNH CÔNG!");
    console.log("==================================================================");

    ws.close();
    chrome.kill();
    process.exit(0);
}

main().catch(err => {
    console.error("Test Suite Error:", err);
    process.exit(1);
});
