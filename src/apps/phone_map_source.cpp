#include "phone_map_source.h"

#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "phone_map_protocol.h"
#include "../os/network_coordinator.h"
#include "../os/runtime_health.h"
#include "../os/wifi_manager.h"
#include "../ui/ui_manager.h"

namespace
{
WebServer s_server(PHONE_MAP_HTTP_PORT);
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;
uint8_t *s_frame_buffer = nullptr;
size_t s_frame_size = 0;
bool s_frame_ready = false;
bool s_request_active = false;
PhoneMapFrameMetadata s_request = {};
char s_pair_code[7] = {};
bool s_routes_registered = false;
bool s_server_started = false;

struct UploadState
{
    bool accepted;
    bool failed;
    bool complete;
    bool bulk_acquired;
    uint32_t generation;
    size_t size;
};
UploadState s_upload = {};

const char PHONE_MAP_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>Nguồn bản đồ ESP32</title><style>
:root{color-scheme:dark;font-family:system-ui,sans-serif}body{margin:0;background:#09111d;color:#eef6ff}
main{max-width:420px;margin:auto;padding:18px}.card{background:#121d2b;border:1px solid #26384e;border-radius:16px;padding:16px}
h1{font-size:20px;margin:0 0 8px}p{color:#a9bad0;line-height:1.4}label{display:block;margin:12px 0 6px}
input,button{box-sizing:border-box;width:100%;height:44px;border-radius:10px;border:1px solid #35506e;background:#0b1522;color:#fff;padding:0 12px;font-size:16px}
button{margin-top:10px;background:#007d8a;border:0;font-weight:700}canvas{display:block;width:240px;height:320px;margin:16px auto 8px;border-radius:10px;background:#0b1522}
#status{text-align:center;min-height:24px;color:#62e7ef}.small{font-size:12px;text-align:center}
</style></head><body><main><div class="card"><h1>Bản đồ từ điện thoại</h1>
<p>Mở ứng dụng Bản đồ trên ESP32, nhập mã ghép nối đang hiển thị rồi giữ trang này mở.</p>
<label for="code">Mã ghép nối</label><input id="code" maxlength="6" autocomplete="one-time-code" autocapitalize="characters">
<button id="connect">Kết nối</button><button id="refresh">Gửi lại ảnh hiện tại</button>
<canvas id="map" width="240" height="320"></canvas><div id="status">Chưa kết nối</div>
<p class="small">Ảnh chứa dữ liệu © OpenStreetMap contributors. Kết nối dùng HTTP nội bộ trong cùng mạng Wi‑Fi.</p>
</div></main><script>
const code=document.querySelector('#code'),statusEl=document.querySelector('#status');
const canvas=document.querySelector('#map'),ctx=canvas.getContext('2d');let token='',lastGeneration=0,lastRequest=null,busy=false;
code.value=localStorage.getItem('esp32-map-code')||'';
function status(t){statusEl.textContent=t} function clamp(v,a,b){return Math.max(a,Math.min(b,v))}
function tilePoint(lat,lon,z){const n=Math.pow(2,z),s=Math.sin(clamp(lat,-85.0511,85.0511)*Math.PI/180);return{x:(lon+180)/360*n*256,y:(.5-Math.log((1+s)/(1-s))/(4*Math.PI))*n*256,n}}
function loadTile(z,x,y,n){return new Promise((resolve,reject)=>{const img=new Image();img.crossOrigin='anonymous';img.onload=()=>resolve(img);img.onerror=reject;const wx=((x%n)+n)%n;img.src=`https://tile.openstreetmap.org/${z}/${wx}/${y}.png`})}
async function renderAndSend(r){if(busy)return;busy=true;try{status('Đang tải tile OpenStreetMap…');ctx.fillStyle='#dce5ed';ctx.fillRect(0,0,240,320);const p=tilePoint(r.lat,r.lon,r.zoom),left=p.x-120,top=p.y-160;
const x0=Math.floor(left/256),x1=Math.floor((left+239)/256),y0=Math.floor(top/256),y1=Math.floor((top+319)/256);const jobs=[];
for(let y=y0;y<=y1;y++)for(let x=x0;x<=x1;x++)if(y>=0&&y<p.n)jobs.push(loadTile(r.zoom,x,y,p.n).then(img=>({img,x,y})));
const tiles=await Promise.all(jobs);for(const t of tiles)ctx.drawImage(t.img,Math.round(t.x*256-left),Math.round(t.y*256-top));
ctx.fillStyle='rgba(0,0,0,.65)';ctx.fillRect(0,301,240,19);ctx.fillStyle='#fff';ctx.font='11px system-ui';ctx.fillText('© OpenStreetMap contributors',6,315);
const blob=await new Promise(resolve=>canvas.toBlob(resolve,'image/jpeg',.82));if(!blob)throw new Error('Không tạo được JPEG');
const form=new FormData();form.append('frame',blob,'map.jpg');status('Đang gửi ảnh tới ESP32…');
const u=`/api/map-frame?token=${encodeURIComponent(token)}&generation=${r.generation}`;const res=await fetch(u,{method:'POST',body:form,cache:'no-store'});if(!res.ok)throw new Error(await res.text()||`HTTP ${res.status}`);
lastGeneration=r.generation;lastRequest=r;status(`Đã gửi bản đồ Z${r.zoom}`)}catch(e){status(`Lỗi: ${e.message}`)}finally{busy=false}}
async function poll(){if(!token||busy)return;try{const res=await fetch(`/api/map-request?token=${encodeURIComponent(token)}&t=${Date.now()}`,{cache:'no-store'});if(res.status===204){status('Hãy mở ứng dụng Bản đồ trên ESP32');return}if(!res.ok)throw new Error(await res.text()||`HTTP ${res.status}`);const r=await res.json();if(r.generation!==lastGeneration)await renderAndSend(r)}catch(e){status(`Mất kết nối: ${e.message}`)}}
document.querySelector('#connect').onclick=async()=>{token=code.value.trim().toUpperCase();if(token.length!==6){status('Mã phải có 6 ký tự');return}localStorage.setItem('esp32-map-code',token);lastGeneration=0;status('Đang mở Bản đồ trên ESP32…');try{const res=await fetch(`/api/open-map?token=${encodeURIComponent(token)}`,{method:'POST',cache:'no-store'});if(!res.ok)throw new Error(await res.text()||`HTTP ${res.status}`);await poll()}catch(e){status(`Không mở được Bản đồ: ${e.message}`)}};
document.querySelector('#refresh').onclick=()=>{if(lastRequest){lastGeneration=0;renderAndSend(lastRequest)}else poll()};setInterval(poll,1000);
</script></body></html>
)HTML";

bool lock_state(TickType_t timeout = pdMS_TO_TICKS(250))
{
    return s_mutex && xSemaphoreTake(s_mutex, timeout) == pdTRUE;
}

void unlock_state()
{
    xSemaphoreGive(s_mutex);
}

void release_upload_lease()
{
    if (!s_upload.bulk_acquired) return;
    network_bulk_release();
    s_upload.bulk_acquired = false;
}

bool token_matches_request()
{
    if (!s_server.hasArg("token")) return false;
    const String supplied = s_server.arg("token");
    return supplied.length() == 6 && supplied.equalsIgnoreCase(s_pair_code);
}

uint32_t requested_generation()
{
    if (!s_server.hasArg("generation")) return 0;
    const String raw = s_server.arg("generation");
    if (raw.length() == 0 || raw.length() > 10) return 0;
    char *end = nullptr;
    const unsigned long parsed = strtoul(raw.c_str(), &end, 10);
    return end && *end == '\0' ? static_cast<uint32_t>(parsed) : 0;
}

void send_no_store(int code, const char *content_type, const String &body)
{
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.sendHeader("X-Content-Type-Options", "nosniff");
    s_server.send(code, content_type, body);
}

void handle_root()
{
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.send_P(200, "text/html; charset=utf-8", PHONE_MAP_PAGE);
}

void handle_request()
{
    if (!token_matches_request())
    {
        send_no_store(403, "text/plain; charset=utf-8", "Mã ghép nối không đúng");
        return;
    }
    if (!network_background_allowed())
    {
        send_no_store(503, "text/plain; charset=utf-8",
                      "Đang ưu tiên thoại hoặc nhạc; sẽ thử lại");
        return;
    }

    PhoneMapFrameMetadata request = {};
    bool active = false;
    if (lock_state())
    {
        active = s_request_active;
        request = s_request;
        unlock_state();
    }
    if (!active)
    {
        send_no_store(204, "text/plain", "");
        return;
    }

    char json[192];
    snprintf(json, sizeof(json),
             "{\"generation\":%lu,\"lat\":%.7f,\"lon\":%.7f,\"zoom\":%d,\"maptype\":\"%s\",\"width\":240,\"height\":320}",
             static_cast<unsigned long>(request.generation), request.lat,
             request.lon, request.zoom, request.maptype);
    send_no_store(200, "application/json", json);
}

void handle_open_map()
{
    if (!token_matches_request())
    {
        send_no_store(403, "text/plain; charset=utf-8", "Mã ghép nối không đúng");
        return;
    }

    if (ui_open_map_app())
        send_no_store(202, "text/plain; charset=utf-8", "Đã mở ứng dụng Bản đồ");
    else
        send_no_store(503, "text/plain; charset=utf-8",
                      "Giao diện đang bận; hãy thử lại");
}

void handle_upload_chunk()
{
    HTTPUpload &upload = s_server.upload();
    if (upload.status == UPLOAD_FILE_START)
    {
        release_upload_lease();
        s_upload = {};
        s_upload.generation = requested_generation();
        if (!token_matches_request())
        {
            s_upload.failed = true;
            return;
        }
        s_upload.bulk_acquired = network_bulk_acquire(0);
        if (!s_upload.bulk_acquired || !lock_state())
        {
            release_upload_lease();
            s_upload.failed = true;
            return;
        }
        s_upload.accepted = s_frame_buffer && phone_map_generation_accepts(
            s_upload.generation, s_request.generation, s_request_active);
        s_frame_size = 0;
        s_frame_ready = false;
        unlock_state();
        if (!s_upload.accepted) s_upload.failed = true;
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE)
    {
        if (!s_upload.accepted || s_upload.failed || upload.currentSize == 0) return;
        if (!lock_state())
        {
            s_upload.failed = true;
            return;
        }
        const bool current = s_frame_buffer && phone_map_generation_accepts(
            s_upload.generation, s_request.generation, s_request_active);
        const bool fits = upload.currentSize <= PHONE_MAP_MAX_JPEG_BYTES - s_upload.size;
        if (current && fits)
        {
            memcpy(s_frame_buffer + s_upload.size, upload.buf, upload.currentSize);
            s_upload.size += upload.currentSize;
        }
        else
        {
            s_upload.failed = true;
        }
        unlock_state();
        return;
    }

    if (upload.status == UPLOAD_FILE_END)
    {
        if (!s_upload.accepted || s_upload.failed || !lock_state())
        {
            s_upload.failed = true;
            release_upload_lease();
            return;
        }
        const bool current = phone_map_generation_accepts(
            s_upload.generation, s_request.generation, s_request_active);
        const bool valid = current && phone_map_jpeg_envelope_valid(
            s_frame_buffer, s_upload.size);
        if (valid)
        {
            s_frame_size = s_upload.size;
            s_frame_ready = true;
            s_upload.complete = true;
        }
        else
        {
            s_upload.failed = true;
            s_frame_size = 0;
            s_frame_ready = false;
        }
        unlock_state();
        release_upload_lease();
        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED)
    {
        s_upload.failed = true;
        if (lock_state())
        {
            s_frame_size = 0;
            s_frame_ready = false;
            unlock_state();
        }
        release_upload_lease();
    }
}

void handle_upload_complete()
{
    if (s_upload.complete && !s_upload.failed)
        send_no_store(202, "text/plain; charset=utf-8", "Đã nhận ảnh bản đồ");
    else if (!token_matches_request())
        send_no_store(403, "text/plain; charset=utf-8", "Mã ghép nối không đúng");
    else
        send_no_store(409, "text/plain; charset=utf-8",
                      "Ảnh không hợp lệ, quá lớn hoặc request đã cũ");
}

void server_task(void *)
{
    for (;;)
    {
        runtime_health_heartbeat(RUNTIME_TASK_PHONE_MAP);
        if (!s_server_started)
        {
            // WebServer::begin() opens a lwIP socket. The WiFi manager starts
            // asynchronously, so opening it before the TCP/IP mailbox exists
            // causes an IDF "Invalid mbox" assertion and a boot loop.
            if (wifi_manager_is_driver_ready())
            {
                s_server.begin();
                s_server_started = true;
                Serial.printf("[PHONE_MAP] LAN companion listening on port %u; pairing code %s\n",
                              static_cast<unsigned>(PHONE_MAP_HTTP_PORT), s_pair_code);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(WiFi.status() == WL_CONNECTED ? 10 : 250));
    }
}
}

bool phone_map_source_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return false;

    if (s_pair_code[0] == '\0')
    {
        const uint32_t random_value = esp_random() & 0xFFFFFFU;
        snprintf(s_pair_code, sizeof(s_pair_code), "%06lX",
                 static_cast<unsigned long>(random_value));
    }

    if (!s_routes_registered)
    {
        s_server.on("/", HTTP_GET, handle_root);
        s_server.on("/phone-map", HTTP_GET, handle_root);
        s_server.on("/api/open-map", HTTP_POST, handle_open_map);
        s_server.on("/api/map-request", HTTP_GET, handle_request);
        s_server.on("/api/map-frame", HTTP_POST,
                    handle_upload_complete, handle_upload_chunk);
        s_server.onNotFound([]() {
            send_no_store(404, "text/plain; charset=utf-8", "Không tìm thấy endpoint");
        });
        s_routes_registered = true;
    }

    if (!s_task)
    {
        if (xTaskCreatePinnedToCore(server_task, "PhoneMapHTTP", 8 * 1024,
                                    nullptr, 1, &s_task, 0) != pdPASS)
        {
            s_task = nullptr;
            return false;
        }
    }

    Serial.printf("[PHONE_MAP] LAN companion queued on port %u; pairing code %s\n",
                  static_cast<unsigned>(PHONE_MAP_HTTP_PORT), s_pair_code);
    return true;
}

bool phone_map_source_set_request(uint32_t generation, double lat, double lon,
                                  int zoom, const char *maptype)
{
    if (!generation || !maptype || !lock_state()) return false;
    if (!s_frame_buffer)
    {
        s_frame_buffer = static_cast<uint8_t *>(heap_caps_malloc(
            PHONE_MAP_MAX_JPEG_BYTES, MALLOC_CAP_SPIRAM));
    }
    if (!s_frame_buffer)
    {
        unlock_state();
        return false;
    }

    s_request.generation = generation;
    s_request.lat = lat;
    s_request.lon = lon;
    s_request.zoom = zoom;
    strlcpy(s_request.maptype, maptype, sizeof(s_request.maptype));
    s_request_active = true;
    s_frame_size = 0;
    s_frame_ready = false;
    unlock_state();
    return true;
}

void phone_map_source_clear_request(void)
{
    if (!lock_state()) return;
    s_request_active = false;
    s_request = {};
    s_frame_size = 0;
    s_frame_ready = false;
    heap_caps_free(s_frame_buffer);
    s_frame_buffer = nullptr;
    unlock_state();
}

bool phone_map_source_take_frame(uint8_t *dest, size_t capacity,
                                 size_t *out_size,
                                 PhoneMapFrameMetadata *out_metadata)
{
    if (!dest || !out_size || !out_metadata || !lock_state()) return false;
    const bool available = s_frame_ready && s_frame_buffer && s_request_active &&
        s_frame_size <= capacity;
    if (!available)
    {
        unlock_state();
        return false;
    }
    memcpy(dest, s_frame_buffer, s_frame_size);
    *out_size = s_frame_size;
    *out_metadata = s_request;
    s_frame_ready = false;
    unlock_state();
    return true;
}

void phone_map_source_get_pairing_hint(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (WiFi.status() != WL_CONNECTED)
    {
        strlcpy(out, "Kết nối WiFi 2.4GHz", out_size);
        return;
    }
    const String ip = WiFi.localIP().toString();
    snprintf(out, out_size, "%s:%u code %s", ip.c_str(),
             static_cast<unsigned>(PHONE_MAP_HTTP_PORT), s_pair_code);
}
