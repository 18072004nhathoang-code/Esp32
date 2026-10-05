#include "ble_remote_service.h"
#include "navigation_service.h"
#include "ble_remote_logic.h"
#include "../audio/music_player.h"
#include "../ai/ai_voice_service.h"
#include "../os/system_info.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>

namespace {
using namespace ble_remote;
constexpr uint16_t kAppId = 0x28;
// 7cf10000-6e6d-4f73-9f2e-455333433238; ...0001 command, ...0002 status.
uint8_t service_uuid[16] = {0x38,0x32,0x43,0x33,0x53,0x45,0x2e,0x9f,0x73,0x4f,0x6d,0x6e,0x00,0x00,0xf1,0x7c};
uint8_t command_uuid[16] = {0x38,0x32,0x43,0x33,0x53,0x45,0x2e,0x9f,0x73,0x4f,0x6d,0x6e,0x01,0x00,0xf1,0x7c};
uint8_t status_uuid[16] = {0x38,0x32,0x43,0x33,0x53,0x45,0x2e,0x9f,0x73,0x4f,0x6d,0x6e,0x02,0x00,0xf1,0x7c};
uint8_t nav_uuid[16] = {0x38,0x32,0x43,0x33,0x53,0x45,0x2e,0x9f,0x73,0x4f,0x6d,0x6e,0x03,0x00,0xf1,0x7c};
uint8_t audio_uuid[16] = {0x38,0x32,0x43,0x33,0x53,0x45,0x2e,0x9f,0x73,0x4f,0x6d,0x6e,0x04,0x00,0xf1,0x7c};
uint8_t ack_uuid[16] = {0x38,0x32,0x43,0x33,0x53,0x45,0x2e,0x9f,0x73,0x4f,0x6d,0x6e,0x05,0x00,0xf1,0x7c};
enum Attribute { Service, CommandDecl, CommandValue, StatusDecl, StatusValue, Cccd, NavDecl, NavValue, AudioDecl, AudioValue, AckDecl, AckValue, Count };
uint16_t handles[Count] = {};
uint16_t primary_uuid = ESP_GATT_UUID_PRI_SERVICE;
uint16_t declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
uint16_t cccd_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
uint8_t write_property = ESP_GATT_CHAR_PROP_BIT_WRITE;
uint8_t read_property = ESP_GATT_CHAR_PROP_BIT_READ;
uint8_t nav_initial[1]={};
uint8_t status_property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
uint8_t initial_command[kCommandBytes] = {}, initial_status[kStatusBytes] = {}, initial_cccd[2] = {};
esp_gatts_attr_db_t attributes[Count] = {
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&primary_uuid), ESP_GATT_PERM_READ, 16, 16, service_uuid}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&declaration_uuid), ESP_GATT_PERM_READ, 1, 1, &write_property}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, command_uuid, ESP_GATT_PERM_WRITE_ENC_MITM, kCommandBytes, kCommandBytes, initial_command}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&declaration_uuid), ESP_GATT_PERM_READ, 1, 1, &status_property}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, status_uuid, ESP_GATT_PERM_READ_ENC_MITM, kStatusBytes, kStatusBytes, initial_status}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&cccd_uuid), ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM, 2, 2, initial_cccd}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&declaration_uuid), ESP_GATT_PERM_READ, 1, 1, &write_property}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, nav_uuid, ESP_GATT_PERM_WRITE_ENC_MITM, 244, 0, nav_initial}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&declaration_uuid), ESP_GATT_PERM_READ, 1, 1, &write_property}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, audio_uuid, ESP_GATT_PERM_WRITE_ENC_MITM, 244, 0, nav_initial}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t *>(&declaration_uuid), ESP_GATT_PERM_READ, 1, 1, &read_property}},
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, ack_uuid, ESP_GATT_PERM_READ_ENC_MITM, 20, 0, nav_initial}}
};
struct Work { Command command; uint32_t epoch; };
QueueHandle_t queue = nullptr;
TaskHandle_t worker_handle = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Gate gate;
BleRemoteSnapshot info = {};
esp_gatt_if_t gatt_if = ESP_GATT_IF_NONE;
uint16_t connection = 0;
esp_bd_addr_t peer = {};
bool connected = false, subscribed = false, congested = false;
bool ready = false, adv_data_ready = false, scan_data_ready = false, advertising = false, adv_pending = false;
std::atomic<bool> controller_owned{false}, host_owned{false};
uint8_t cached[kStatusBytes] = {1, 32};
uint16_t last_id = 0;
Result last_result = Result::Status;
uint32_t start_ms = 0, notify_ms = 0;
std::atomic<unsigned> cleanup_attempts{0};

bool check(esp_err_t result, const char *operation) {
    if (result == ESP_OK) return true;
    portENTER_CRITICAL(&mux);
    snprintf(info.error, sizeof(info.error), "BLE %s: %s", operation, esp_err_to_name(result));
    info.phase = BleRemotePhase::Error;
    info.requested = false;
    gate.disconnect();
    portEXIT_CRITICAL(&mux);
    Serial.printf("[BLE] %s failed (%d)\n", operation, static_cast<int>(result));
    return false;
}
bool desired() { portENTER_CRITICAL(&mux); bool value = info.requested; portEXIT_CRITICAL(&mux); return value; }
bool current(uint32_t epoch) {
    portENTER_CRITICAL(&mux);
    bool value = info.requested && gate.authorized(epoch);
    portEXIT_CRITICAL(&mux);
    return value;
}

class DevicePorts : public Ports {
public:
    explicit DevicePorts(uint32_t epoch) : epoch_(epoch) {}
    State snapshot() override {
        MusicPlayerState player = {};
        State s;
        s.available = music_player_copy_state(&player);
        if (s.available) {
            s.playing = player.is_playing && !player.is_paused;
            s.paused = player.is_playing && player.is_paused;
            s.volume = player.volume;
            s.track = static_cast<int16_t>(player.current_track_idx);
            s.tracks = static_cast<uint16_t>(player.total_tracks);
            s.position = player.current_time_sec;
            s.duration = player.total_duration_sec;
        }
        SystemStats stats = system_get_stats();
        s.wifi = stats.wifi_connected; s.sd = stats.storage_available;
        const AIVoiceState ai = ai_voice_get_state();
        s.busy = ai_voice_get_active_generation() != 0 ||
            (ai >= AI_STATE_STARTING && ai <= AI_STATE_CANCELING);
        return s;
    }
    bool apply(Op op, uint16_t value) override {
        // Recheck at execution, not only when the callback enqueued the packet.
        if (!current(epoch_) || snapshot().busy) return false;
        if (op == Op::Play) return music_player_play_index_wait(value, 2500);
        if (op == Op::Resume && !music_player_is_paused()) {
            MusicVoiceHandoff bookmark = {};
            if (ai_voice_copy_paused_music(&bookmark)) {
                char error[96] = {};
                return music_player_restore_after_voice(&bookmark, 3000, error, sizeof(error));
            }
        }
        AiMusicAction action = {};
        switch (op) {
            case Op::Pause: action.type = AI_MUSIC_ACTION_PAUSE; break;
            case Op::Resume: action.type = AI_MUSIC_ACTION_RESUME; break;
            case Op::Stop: action.type = AI_MUSIC_ACTION_STOP; break;
            case Op::Volume: action.type = AI_MUSIC_ACTION_VOLUME; action.volume = static_cast<uint8_t>(value); break;
            default: return false;
        }
        char error[96] = {};
        return music_player_execute_ai_action(&action, 2500, error, sizeof(error));
    }
private:
    uint32_t epoch_;
};

void send_response(esp_gatt_if_t interface, uint16_t conn, uint32_t transaction,
                   esp_gatt_status_t status, esp_gatt_rsp_t *response = nullptr) {
    // A peer can disconnect while the stack is submitting this response. Do not
    // tear down the whole service for a failed reply to an already-dead peer.
    esp_err_t result = esp_ble_gatts_send_response(interface, conn, transaction, status, response);
    if (result != ESP_OK) Serial.printf("[BLE] ATT response not delivered (%d)\n", static_cast<int>(result));
}
void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            if (check(p->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS ? ESP_OK : ESP_FAIL, "advertisement")) {
                portENTER_CRITICAL(&mux); adv_data_ready = true; portEXIT_CRITICAL(&mux);
            }
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            portENTER_CRITICAL(&mux);
            adv_pending = false;
            advertising = p->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
            if (advertising && info.requested && !connected) info.phase = BleRemotePhase::Advertising;
            portEXIT_CRITICAL(&mux);
            if (p->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) (void)check(ESP_FAIL, "advertising start");
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            if (check(p->scan_rsp_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS ? ESP_OK : ESP_FAIL, "scan response ready")) {
                portENTER_CRITICAL(&mux); scan_data_ready = true; portEXIT_CRITICAL(&mux);
            }
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            portENTER_CRITICAL(&mux); advertising = adv_pending = false; portEXIT_CRITICAL(&mux);
            break;
        case ESP_GAP_BLE_SEC_REQ_EVT: {
            portENTER_CRITICAL(&mux);
            bool accept = info.requested && connected && memcmp(peer, p->ble_security.ble_req.bd_addr, sizeof(peer)) == 0;
            portEXIT_CRITICAL(&mux);
            (void)check(esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, accept), "security reply");
            break;
        }
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            portENTER_CRITICAL(&mux);
            if (info.requested && connected && memcmp(peer, p->ble_security.key_notif.bd_addr, sizeof(peer)) == 0) {
                info.passkey = p->ble_security.key_notif.passkey;
                info.passkey_visible = true;
                info.phase = BleRemotePhase::Pairing;
            }
            portEXIT_CRITICAL(&mux);
            break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            auto &auth = p->ble_security.auth_cmpl;
            const bool secure = auth.success && (auth.auth_mode & ESP_LE_AUTH_REQ_SC_MITM) == ESP_LE_AUTH_REQ_SC_MITM;
            bool matched = false;
            portENTER_CRITICAL(&mux);
            matched = connected && memcmp(peer, auth.bd_addr, sizeof(peer)) == 0;
            if (matched) {
                gate.authenticate(info.requested && secure);
                info.passkey_visible = false;
                if (info.requested && secure) info.phase = BleRemotePhase::Connected;
            }
            portEXIT_CRITICAL(&mux);
            if (matched && !secure) (void)check(esp_ble_gap_disconnect(auth.bd_addr), "reject insecure pairing");
            break;
        }
        default: break;
    }
}

void gatt_callback(esp_gatts_cb_event_t event, esp_gatt_if_t interface, esp_ble_gatts_cb_param_t *p) {
    switch (event) {
        case ESP_GATTS_REG_EVT:
            if (!check(p->reg.status == ESP_GATT_OK ? ESP_OK : ESP_FAIL, "register")) break;
            portENTER_CRITICAL(&mux); gatt_if = interface; portEXIT_CRITICAL(&mux);
            (void)check(esp_ble_gatts_create_attr_tab(attributes, interface, Count, 0), "attribute table");
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (!check(p->add_attr_tab.status == ESP_GATT_OK && p->add_attr_tab.num_handle == Count ? ESP_OK : ESP_FAIL, "attributes")) break;
            portENTER_CRITICAL(&mux); memcpy(handles, p->add_attr_tab.handles, sizeof(handles)); portEXIT_CRITICAL(&mux);
            (void)check(esp_ble_gatts_start_service(handles[Service]), "service start");
            break;
        case ESP_GATTS_START_EVT:
            if (check(p->start.status == ESP_GATT_OK ? ESP_OK : ESP_FAIL, "service ready")) {
                portENTER_CRITICAL(&mux); ready = true; portEXIT_CRITICAL(&mux);
            }
            break;
        case ESP_GATTS_CONNECT_EVT: {
            bool accept = false;
            portENTER_CRITICAL(&mux);
            accept = info.requested && !connected;
            if (accept) {
                connected = true; subscribed = congested = advertising = adv_pending = false;
                connection = p->connect.conn_id;
                memcpy(peer, p->connect.remote_bda, sizeof(peer));
                gate.connect(); last_id = 0; last_result = Result::Status;
                cached[2] = cached[3] = cached[4] = 0;
                for (unsigned i = 0; i < 4; ++i) cached[16 + i] = static_cast<uint8_t>(gate.epoch() >> (8 * i));
                info.phase = BleRemotePhase::Pairing;
                info.passkey_visible = false;
                start_ms = millis();
            }
            portEXIT_CRITICAL(&mux);
            if (accept) (void)check(esp_ble_set_encryption(p->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM), "encryption");
            else (void)check(esp_ble_gatts_close(interface, p->connect.conn_id), "reject second peer");
            break;
        }
        case ESP_GATTS_DISCONNECT_EVT:
            navigation_disconnect();
            portENTER_CRITICAL(&mux);
            if (connected && connection == p->disconnect.conn_id) {
                gate.disconnect(); connected = subscribed = congested = false;
                info.passkey_visible = false;
                if (info.requested) { info.phase = BleRemotePhase::Starting; start_ms = millis(); }
            }
            portEXIT_CRITICAL(&mux);
            break;
        case ESP_GATTS_CONGEST_EVT:
            portENTER_CRITICAL(&mux);
            if (connected && connection == p->congest.conn_id) congested = p->congest.congested;
            portEXIT_CRITICAL(&mux);
            break;
        case ESP_GATTS_READ_EVT: {
            esp_gatt_rsp_t response = {};
            response.attr_value.handle = p->read.handle;
            response.attr_value.offset = p->read.offset;
            esp_gatt_status_t status = ESP_GATT_OK;
            uint8_t nav_ack[20]; navigation_ack(nav_ack);
            portENTER_CRITICAL(&mux);
            if (!info.requested || !connected || p->read.conn_id != connection || !gate.authorized(gate.epoch())) status = ESP_GATT_INSUF_AUTHENTICATION;
            else if (p->read.handle == handles[StatusValue] || p->read.handle == handles[Cccd] || p->read.handle == handles[AckValue]) {
                const size_t size = p->read.handle == handles[Cccd] ? 2 : 20;
                if (p->read.offset > size) status = ESP_GATT_INVALID_OFFSET;
                else {
                    uint8_t cccd[2] = {static_cast<uint8_t>(subscribed), 0};
                    const uint8_t *value = p->read.handle == handles[StatusValue] ? cached : (p->read.handle == handles[AckValue] ? nav_ack : cccd);
                    response.attr_value.len = static_cast<uint16_t>(size - p->read.offset);
                    memcpy(response.attr_value.value, value + p->read.offset, response.attr_value.len);
                }
            } else status = ESP_GATT_READ_NOT_PERMIT;
            portEXIT_CRITICAL(&mux);
            send_response(interface, p->read.conn_id, p->read.trans_id, status, &response);
            break;
        }
        case ESP_GATTS_WRITE_EVT: {
            esp_gatt_status_t status = ESP_GATT_OK;
            Work work = {};
            portENTER_CRITICAL(&mux);
            const bool allowed = info.requested && connected && p->write.conn_id == connection && gate.authorized(gate.epoch());
            portEXIT_CRITICAL(&mux);
            if (!allowed) status = ESP_GATT_INSUF_AUTHENTICATION;
            else if (p->write.is_prep) status = ESP_GATT_REQ_NOT_SUPPORTED;
            else if (p->write.offset != 0) status = ESP_GATT_INVALID_OFFSET;
            else if (p->write.handle == handles[Cccd]) {
                if (p->write.len != 2 || p->write.value[1] != 0 || p->write.value[0] > 1) status = ESP_GATT_INVALID_ATTR_LEN;
                else { portENTER_CRITICAL(&mux); subscribed = p->write.value[0] == 1; portEXIT_CRITICAL(&mux); }
            } else if (p->write.handle == handles[NavValue] || p->write.handle == handles[AudioValue]) {
                const bool audio=p->write.handle==handles[AudioValue];
                if(p->write.len<2 || (p->write.value[1]==navigation::Audio)!=audio || !navigation_receive(p->write.value,p->write.len))
                    status=ESP_GATT_INSUF_RESOURCE;
            } else if (p->write.handle == handles[CommandValue]) {
                if (!decode(p->write.value, p->write.len, work.command)) status = ESP_GATT_INVALID_ATTR_LEN;
                else {
                    portENTER_CRITICAL(&mux);
                    bool reserved = gate.reserve(work.command.id, work.epoch);
                    portEXIT_CRITICAL(&mux);
                    if (!reserved) status = ESP_GATT_INSUF_RESOURCE;
                    else if (!queue || xQueueSend(queue, &work, 0) != pdTRUE) {
                        portENTER_CRITICAL(&mux); gate.rollback(work.epoch); portEXIT_CRITICAL(&mux);
                        status = ESP_GATT_INSUF_RESOURCE;
                    }
                }
            } else status = ESP_GATT_WRITE_NOT_PERMIT;
            if (p->write.need_rsp) send_response(interface, p->write.conn_id, p->write.trans_id, status);
            break;
        }
        case ESP_GATTS_EXEC_WRITE_EVT:
            send_response(interface, p->exec_write.conn_id, p->exec_write.trans_id, ESP_GATT_REQ_NOT_SUPPORTED);
            break;
        default: break;
    }
}

void stop_stack() {
    navigation_disconnect();
    portENTER_CRITICAL(&mux);
    gate.disconnect(); subscribed = false; info.passkey_visible = false;
    portEXIT_CRITICAL(&mux);
    // Disable/deinit only resources this service acquired. Both calls are made
    // by the sole worker, never by LVGL or a Bluetooth callback.
    bool ok = true;
    if (host_owned) {
        if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED)
            ok = check(esp_bluedroid_disable(), "host disable") && ok;
        if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED)
            ok = check(esp_bluedroid_deinit(), "host deinit") && ok;
        host_owned = esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED;
    }
    if (!host_owned && controller_owned) {
        if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
            ok = check(esp_bt_controller_disable(), "radio disable") && ok;
        if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
            ok = check(esp_bt_controller_deinit(), "radio deinit") && ok;
        controller_owned = esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE;
    }
    portENTER_CRITICAL(&mux);
    connected = subscribed = ready = adv_data_ready = scan_data_ready = advertising = adv_pending = false;
    gatt_if = ESP_GATT_IF_NONE;
    if (ok && info.phase != BleRemotePhase::Error) info.phase = BleRemotePhase::Off;
    portEXIT_CRITICAL(&mux);
}

bool start_stack() {
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE ||
        esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED)
        return check(ESP_ERR_INVALID_STATE, "radio already owned");
    // The pinned SDK's Bluedroid/controller allocations are internal SRAM.
    // Fail closed rather than steal the RAM needed by audio, WiFi and TLS.
    Serial.printf("[BLE] Init internal_free=%u largest=%u\n",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    if (!startup_ram_ok(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)))
        return check(ESP_ERR_NO_MEM, "dung nhac/AI roi bat lai");
    portENTER_CRITICAL(&mux);
    info.phase = BleRemotePhase::Starting; info.error[0] = '\0';
    ready = adv_data_ready = scan_data_ready = advertising = adv_pending = false;
    portEXIT_CRITICAL(&mux);
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    // This peripheral only needs one advertiser and one connection, not the
    // SDK default six activities. Keep the post-init audio/WiFi reserve below;
    // admission alone is never evidence that the running stack fits in SRAM.
    config.ble_max_act = 2;
    if (!check(esp_bt_controller_init(&config), "radio init")) return false;
    controller_owned = true;
    if (!check(esp_bt_controller_enable(ESP_BT_MODE_BLE), "radio enable") ||
        !check(esp_bluedroid_init(), "host init")) return false;
    host_owned = true;
    if (!check(esp_bluedroid_enable(), "host enable")) return false;
    Serial.printf("[BLE] Allocated internal_free=%u largest=%u\n",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    if (!runtime_ram_ok(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)))
        return check(ESP_ERR_NO_MEM, "audio/WiFi RAM reserve");
    uint8_t auth = ESP_LE_AUTH_REQ_SC_MITM; // No bonding/NVS; fresh pairing per connection.
    uint8_t io = ESP_IO_CAP_OUT, key_size = 16, strict = 1;
    if (!check(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth)), "secure pairing") ||
        !check(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, sizeof(io)), "passkey display") ||
        !check(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)), "key size") ||
        !check(esp_ble_gap_set_security_param(ESP_BLE_SM_MIN_KEY_SIZE, &key_size, sizeof(key_size)), "minimum key size") ||
        !check(esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &strict, sizeof(strict)), "pairing policy") ||
        !check(esp_ble_gap_register_callback(gap_callback), "GAP callback") ||
        !check(esp_ble_gatts_register_callback(gatt_callback), "GATT callback") ||
        !check(esp_ble_gap_set_device_name(info.name), "name")) return false;
    uint8_t advert[21] = {2, ESP_BLE_AD_TYPE_FLAG, ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT, 17, ESP_BLE_AD_TYPE_128SRV_CMPL};
    memcpy(advert + 5, service_uuid, sizeof(service_uuid));
    uint8_t scan[31] = {};
    const size_t name_length = strlen(info.name);
    scan[0] = static_cast<uint8_t>(name_length + 1); scan[1] = ESP_BLE_AD_TYPE_NAME_CMPL;
    memcpy(scan + 2, info.name, name_length);
    if (!check(esp_ble_gap_config_adv_data_raw(advert, sizeof(advert)), "advertisement data") ||
        !check(esp_ble_gap_config_scan_rsp_data_raw(scan, name_length + 2), "scan response") ||
        !check(esp_ble_gatts_app_register(kAppId), "GATT app")) return false;
    portENTER_CRITICAL(&mux); start_ms = millis(); portEXIT_CRITICAL(&mux);
    Serial.printf("[BLE] Starting SD remote; WiFi unchanged; internal_free=%u largest=%u\n",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    return true;
}

void worker(void *) {
    for (;;) {
        if (desired() && !controller_owned && !host_owned) {
            cleanup_attempts = 0;
            if (!start_stack()) stop_stack();
        } else if (!desired() && (controller_owned || host_owned) && cleanup_attempts < 3) {
            ++cleanup_attempts;
            stop_stack();
        }
        bool start_adv = false, timeout = false, pair_timeout = false;
        esp_bd_addr_t timed_out_peer = {};
        portENTER_CRITICAL(&mux);
        if (!info.requested && !controller_owned && !host_owned && info.phase == BleRemotePhase::Stopping)
            info.phase = BleRemotePhase::Off;
        if (info.requested && ready && adv_data_ready && scan_data_ready && !connected && !advertising && !adv_pending) {
            adv_pending = true; start_adv = true;
        }
        timeout = info.requested && info.phase == BleRemotePhase::Starting && millis() - start_ms > 10000;
        pair_timeout = info.requested && connected && info.phase == BleRemotePhase::Pairing && millis() - start_ms > 60000;
        if (pair_timeout) { memcpy(timed_out_peer, peer, sizeof(peer)); start_ms = millis(); }
        portEXIT_CRITICAL(&mux);
        if (timeout) (void)check(ESP_ERR_TIMEOUT, "startup deadline");
        if (pair_timeout) (void)check(esp_ble_gap_disconnect(timed_out_peer), "pairing timeout disconnect");
        if (start_adv) {
            esp_ble_adv_params_t adv = {};
            adv.adv_int_min = 0x100; adv.adv_int_max = 0x180;
            adv.adv_type = ADV_TYPE_IND; adv.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
            adv.channel_map = ADV_CHNL_ALL; adv.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
            (void)check(esp_ble_gap_start_advertising(&adv), "advertise");
        }
        Work work = {};
        const bool got = xQueueReceive(queue, &work, pdMS_TO_TICKS(100)) == pdTRUE;
        if (got && current(work.epoch)) {
            DevicePorts ports(work.epoch);
            State state;
            Result result = execute(work.command, ports, state);
            bool published = false;
            portENTER_CRITICAL(&mux);
            if (info.requested && gate.finish(work.epoch, work.command.id)) {
                last_id = work.command.id; last_result = result;
                state.session = work.epoch;
                encode(state, last_id, last_result, cached);
                notify_ms = 0;
                published = true;
            }
            portEXIT_CRITICAL(&mux);
            if (published) Serial.printf("[BLE] session=%u command=%u op=%u result=%u stack_free=%u\n",
                static_cast<unsigned>(work.epoch), static_cast<unsigned>(work.command.id),
                static_cast<unsigned>(work.command.op), static_cast<unsigned>(result),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }
        if (millis() - notify_ms >= 1000U) {
            DevicePorts ports(0);
            State state = ports.snapshot();
            uint8_t packet[kStatusBytes];
            esp_gatt_if_t interface;
            uint16_t conn, handle;
            bool notify = false;
            portENTER_CRITICAL(&mux);
            state.session = gate.epoch();
            encode(state, last_id, last_result, cached);
            memcpy(packet, cached, sizeof(packet));
            interface = gatt_if; conn = connection; handle = handles[StatusValue];
            notify = info.requested && gate.authorized(gate.epoch()) && subscribed && !congested;
            portEXIT_CRITICAL(&mux);
            if (notify) {
                const esp_err_t result = esp_ble_gatts_send_indicate(interface, conn, handle, sizeof(packet), packet, false);
                if (result != ESP_OK) Serial.printf("[BLE] Notification not delivered (%d); read status to recover\n", static_cast<int>(result));
            }
            notify_ms = millis();
        }
    }
}
} // namespace

bool ble_remote_init() {
    if (worker_handle) return true;
    if (!navigation_init()) Serial.println("[NAV] DEGRADED: worker unavailable");
    uint8_t mac[6] = {};
    if (!check(esp_read_mac(mac, ESP_MAC_BT), "device identity")) return false;
    snprintf(info.name, sizeof(info.name), "MiniOS-%02X%02X%02X", mac[3], mac[4], mac[5]);
    queue = xQueueCreate(2, sizeof(Work));
    if (!queue) return check(ESP_ERR_NO_MEM, "command queue");
    if (xTaskCreatePinnedToCore(worker, "BleRemote", 4096, nullptr, 1, &worker_handle, 0) != pdPASS) {
        vQueueDelete(queue); queue = nullptr;
        return check(ESP_ERR_NO_MEM, "worker task");
    }
    return true;
}
void ble_remote_set_enabled(bool enabled) {
    portENTER_CRITICAL(&mux);
    if (cleanup_attempts >= 3 && (controller_owned || host_owned)) {
        info.phase = BleRemotePhase::Error; info.requested = false;
        strlcpy(info.error, "BLE chua nha tai nguyen; khoi dong lai", sizeof(info.error));
    } else if (enabled && (controller_owned || host_owned) &&
               (info.phase == BleRemotePhase::Stopping || info.phase == BleRemotePhase::Error)) {
        info.requested = false; // Never restart while failed teardown still owns the driver.
    } else if (!worker_handle) {
        info.phase = BleRemotePhase::Error; info.requested = false;
    } else {
        info.requested = enabled;
        if (!enabled) { gate.disconnect(); info.phase = BleRemotePhase::Stopping; info.passkey_visible = false; }
        else if (info.phase == BleRemotePhase::Off || info.phase == BleRemotePhase::Error) info.phase = BleRemotePhase::Starting;
    }
    portEXIT_CRITICAL(&mux);
}
BleRemoteSnapshot ble_remote_snapshot() {
    portENTER_CRITICAL(&mux); BleRemoteSnapshot value = info; portEXIT_CRITICAL(&mux);
    return value;
}
