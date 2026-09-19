#include "xiaozhi_transport.h"

#include <esp_heap_caps.h>

namespace
{
bool parse_wss_url(const char *url, String &host, uint16_t &port, String &path)
{
    if (!xiaozhi::is_secure_websocket_url(url)) return false;
    String remainder = String(url + 6);
    const int slash = remainder.indexOf('/');
    String authority = slash >= 0 ? remainder.substring(0, slash) : remainder;
    path = slash >= 0 ? remainder.substring(slash) : "/";
    if (authority.length() == 0 || authority.indexOf('@') >= 0 || authority.indexOf('#') >= 0)
        return false;
    port = 443;
    const int colon = authority.lastIndexOf(':');
    if (colon > 0)
    {
        const String port_text = authority.substring(colon + 1);
        for (size_t i = 0; i < port_text.length(); ++i)
            if (port_text[i] < '0' || port_text[i] > '9') return false;
        const long parsed = port_text.toInt();
        if (parsed <= 0 || parsed > 65535) return false;
        port = static_cast<uint16_t>(parsed);
        host = authority.substring(0, colon);
    }
    else host = authority;
    return host.length() > 0 && path.length() > 0;
}
}

XiaozhiTransport::XiaozhiTransport()
    : websocket_(nullptr), uplink_queue_(nullptr), inbound_queue_(nullptr), fragment_storage_(nullptr),
      fragment_assembler_(nullptr), connected_(false), generation_(0),
      dropped_uplink_(0), dropped_downlink_(0) {}

XiaozhiTransport::~XiaozhiTransport()
{
    close();
    if (uplink_queue_) vQueueDelete(uplink_queue_);
    if (inbound_queue_) vQueueDelete(inbound_queue_);
    delete fragment_assembler_;
    free(fragment_storage_);
}

bool XiaozhiTransport::begin(const xiaozhi::ProvisionedWebsocket &config,
                             const char *ca_cert, const char *device_id,
                             const char *client_id, uint32_t generation,
                             char *error, size_t error_size)
{
    if (connected() && websocket_ && uri_ == config.url && generation != 0)
    {
        if (setGeneration(generation))
        {
            return true;
        }
    }
    close();
    if (!xiaozhi::valid_websocket_config(config) || !ca_cert || !*ca_cert ||
        !device_id || !*device_id || !client_id || !*client_id || generation == 0)
    {
        if (error && error_size) strlcpy(error, "Cấu hình WSS Xiaozhi không hợp lệ", error_size);
        return false;
    }
    if (!uplink_queue_) uplink_queue_ = xQueueCreate(8, sizeof(AudioPacket));
    if (!inbound_queue_) inbound_queue_ = xQueueCreate(8, sizeof(InboundMessage *));
    if (!fragment_storage_)
        fragment_storage_ = static_cast<uint8_t *>(heap_caps_malloc(
            xiaozhi::kMaxJsonMessageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!fragment_storage_)
        fragment_storage_ = static_cast<uint8_t *>(malloc(xiaozhi::kMaxJsonMessageBytes));
    if (!fragment_assembler_ && fragment_storage_)
        fragment_assembler_ = new xiaozhi::FragmentAssembler(
            fragment_storage_, xiaozhi::kMaxJsonMessageBytes);
    if (!uplink_queue_ || !inbound_queue_ || !fragment_storage_ || !fragment_assembler_)
    {
        if (error && error_size) strlcpy(error, "Thiếu RAM cho queue WebSocket Xiaozhi", error_size);
        return false;
    }

    String host;
    String path;
    uint16_t port = 443;
    if (!parse_wss_url(config.url, host, port, path))
    {
        if (error && error_size) strlcpy(error, "URL WSS Xiaozhi không hợp lệ", error_size);
        return false;
    }
    generation_ = generation;
    dropped_uplink_ = 0;
    dropped_downlink_ = 0;
    String authorization = config.token;
    if (authorization.indexOf(' ') < 0) authorization = "Bearer " + authorization;
    uri_ = config.url;
    headers_ = "Authorization: " + authorization + "\r\n";
    headers_ += "Protocol-Version: " + String(config.version) + "\r\n";
    headers_ += "Device-Id: " + String(device_id) + "\r\n";
    headers_ += "Client-Id: " + String(client_id) + "\r\n";
    esp_websocket_client_config_t ws_config = {};
    ws_config.uri = uri_.c_str();
    ws_config.cert_pem = ca_cert;
    ws_config.headers = headers_.c_str();
    ws_config.disable_auto_reconnect = true;
    ws_config.ping_interval_sec = 15;
    ws_config.pingpong_timeout_sec = 5;
    ws_config.task_prio = 3;
    ws_config.task_stack = 6144;
    ws_config.buffer_size = 2048;
    ws_config.user_context = this;
    websocket_ = esp_websocket_client_init(&ws_config);
    if (!websocket_ || esp_websocket_register_events(
            websocket_, WEBSOCKET_EVENT_ANY, eventHandler, this) != ESP_OK ||
        esp_websocket_client_start(websocket_) != ESP_OK)
    {
        if (websocket_) esp_websocket_client_destroy(websocket_);
        websocket_ = nullptr;
        if (error && error_size) strlcpy(error, "Không khởi động được esp_websocket_client", error_size);
        return false;
    }
    return true;
}

void XiaozhiTransport::loop()
{
    if (!connected() || !uplink_queue_ || !websocket_) return;
    AudioPacket packet = {};
    if (xQueuePeek(uplink_queue_, &packet, 0) == pdTRUE)
    {
        if (packet.generation != generation())
        {
            xQueueReceive(uplink_queue_, &packet, 0);
            in_flight_started_ms_ = 0;
            return;
        }
        const uint32_t now = millis();
        if (in_flight_started_ms_ == 0)
        {
            in_flight_started_ms_ = now == 0 ? 1 : now;
        }
        const int sent = esp_websocket_client_send_bin(
            websocket_, reinterpret_cast<const char *>(packet.data), packet.size,
            pdMS_TO_TICKS(30));
        if (sent == packet.size)
        {
            xQueueReceive(uplink_queue_, &packet, 0);
            in_flight_started_ms_ = 0;
        }
        else
        {
            if (static_cast<int32_t>(now - in_flight_started_ms_) >= 1500)
            {
                xQueueReceive(uplink_queue_, &packet, 0);
                in_flight_started_ms_ = 0;
                dropped_uplink_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    else
    {
        in_flight_started_ms_ = 0;
    }
}

void XiaozhiTransport::close()
{
    connected_ = false;
    if (websocket_)
    {
        (void)esp_websocket_client_stop(websocket_);
        (void)esp_websocket_client_destroy(websocket_);
        websocket_ = nullptr;
    }
    if (uplink_queue_) xQueueReset(uplink_queue_);
    clearInbound();
    if (fragment_assembler_) fragment_assembler_->reset();
    generation_ = 0;
    in_flight_started_ms_ = 0;
}

void XiaozhiTransport::purgeUplink()
{
    if (uplink_queue_) xQueueReset(uplink_queue_);
    in_flight_started_ms_ = 0;
}

bool XiaozhiTransport::setGeneration(uint32_t generation)
{
    if (!connected() || !websocket_ || generation == 0) return false;
    purgeUplink();
    clearInbound();
    if (fragment_assembler_) fragment_assembler_->reset();
    generation_ = generation;
    dropped_uplink_ = 0;
    dropped_downlink_ = 0;
    in_flight_started_ms_ = 0;
    return true;
}

bool XiaozhiTransport::sendText(const char *text)
{
    if (!connected() || !websocket_ || !text || !*text ||
        strlen(text) > xiaozhi::kMaxJsonMessageBytes) return false;
    const int length = static_cast<int>(strlen(text));
    return esp_websocket_client_send_text(websocket_, text, length,
                                          pdMS_TO_TICKS(200)) == length;
}

bool XiaozhiTransport::queueAudio(const uint8_t *data, size_t size, uint32_t generation)
{
    if (!connected() || !uplink_queue_ || !data || size == 0 ||
        size > sizeof(AudioPacket::data) || generation != this->generation()) return false;
    AudioPacket packet = {};
    packet.generation = generation;
    packet.size = static_cast<uint16_t>(size);
    memcpy(packet.data, data, size);
    if (xQueueSend(uplink_queue_, &packet, 0) != pdTRUE)
        return false;
    return true;
}

size_t XiaozhiTransport::uplinkPending() const
{
    return uplink_queue_ ? static_cast<size_t>(uxQueueMessagesWaiting(uplink_queue_)) : 0;
}

size_t XiaozhiTransport::inboundPending() const
{
    return inbound_queue_ ? static_cast<size_t>(uxQueueMessagesWaiting(inbound_queue_)) : 0;
}

bool XiaozhiTransport::audioQueueHasCapacity(uint32_t generation) const
{
    return connected() && uplink_queue_ && generation != 0 && generation == this->generation() &&
           uxQueueSpacesAvailable(uplink_queue_) > 0;
}

bool XiaozhiTransport::enqueueInbound(XiaozhiInboundKind kind,
                                      const uint8_t *data, size_t size)
{
    if (!inbound_queue_ || !data || size == 0 || size > xiaozhi::kMaxJsonMessageBytes)
        return false;
    const size_t allocation = sizeof(InboundMessage) + size;
    InboundMessage *message = static_cast<InboundMessage *>(heap_caps_malloc(
        allocation, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!message) message = static_cast<InboundMessage *>(malloc(allocation));
    if (!message)
    {
        dropped_downlink_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    message->kind = kind;
    message->generation = generation();
    message->size = size;
    memcpy(message->data, data, size);
    if (xQueueSend(inbound_queue_, &message, 0) != pdTRUE)
    {
        free(message);
        dropped_downlink_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool XiaozhiTransport::receive(XiaozhiInboundKind *kind, uint8_t *data,
                               size_t capacity, size_t *size, uint32_t *generation)
{
    if (!kind || !data || !size || !generation || !inbound_queue_) return false;
    InboundMessage *message = nullptr;
    if (xQueueReceive(inbound_queue_, &message, 0) != pdTRUE || !message) return false;
    const bool fits = message->size <= capacity;
    if (fits)
    {
        *kind = message->kind;
        *size = message->size;
        *generation = message->generation;
        memcpy(data, message->data, message->size);
    }
    else dropped_downlink_.fetch_add(1, std::memory_order_relaxed);
    free(message);
    return fits;
}

void XiaozhiTransport::clearInbound()
{
    if (!inbound_queue_) return;
    InboundMessage *message = nullptr;
    while (xQueueReceive(inbound_queue_, &message, 0) == pdTRUE) free(message);
}

void XiaozhiTransport::eventHandler(void *arg, esp_event_base_t, int32_t event_id, void *event_data)
{
    XiaozhiTransport *self = static_cast<XiaozhiTransport *>(arg);
    if (self) self->onEvent(event_id, static_cast<esp_websocket_event_data_t *>(event_data));
}

void XiaozhiTransport::onEvent(int32_t event_id, esp_websocket_event_data_t *event)
{
    if (event_id == WEBSOCKET_EVENT_CONNECTED)
    {
        connected_.store(true, std::memory_order_release);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_ERROR ||
        event_id == WEBSOCKET_EVENT_CLOSED)
    {
        connected_.store(false, std::memory_order_release);
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || !event || !event->data_ptr ||
        event->data_len <= 0 || event->payload_len <= 0 || event->payload_offset < 0 ||
        !fragment_assembler_) return;
    const size_t total = static_cast<size_t>(event->payload_len);
    const size_t offset = static_cast<size_t>(event->payload_offset);
    const size_t length = static_cast<size_t>(event->data_len);
    if (total > xiaozhi::kMaxJsonMessageBytes || offset > total || length > total - offset)
    {
        fragment_assembler_->reset();
        dropped_downlink_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (offset == 0 && event->op_code != 0x1 && event->op_code != 0x2)
    {
        fragment_assembler_->reset();
        dropped_downlink_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const XiaozhiInboundKind inbound_kind = event->op_code == 0x1
        ? XiaozhiInboundKind::TEXT : XiaozhiInboundKind::BINARY;
    const uint8_t *payload = reinterpret_cast<const uint8_t *>(event->data_ptr);
    if (offset == 0 && length == total)
    {
        (void)enqueueInbound(inbound_kind, payload, length);
        return;
    }
    if (offset == 0)
    {
        const xiaozhi::FragmentAssembler::Kind kind = inbound_kind == XiaozhiInboundKind::TEXT
            ? xiaozhi::FragmentAssembler::Kind::TEXT : xiaozhi::FragmentAssembler::Kind::BINARY;
        if (!fragment_assembler_->begin(kind, payload, length))
        {
            fragment_assembler_->reset();
            dropped_downlink_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else if (offset + length < total)
    {
        if (!fragment_assembler_->append(payload, length))
        {
            fragment_assembler_->reset();
            dropped_downlink_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else
    {
        xiaozhi::FragmentAssembler::Kind kind = xiaozhi::FragmentAssembler::Kind::NONE;
        const uint8_t *message = nullptr;
        size_t size = 0;
        if (fragment_assembler_->finish(payload, length, &kind, &message, &size))
            (void)enqueueInbound(kind == xiaozhi::FragmentAssembler::Kind::TEXT
                ? XiaozhiInboundKind::TEXT : XiaozhiInboundKind::BINARY, message, size);
        else dropped_downlink_.fetch_add(1, std::memory_order_relaxed);
        fragment_assembler_->reset();
    }
}
