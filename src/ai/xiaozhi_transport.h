#pragma once

#include <Arduino.h>
#include <atomic>
#include <esp_event.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "xiaozhi_protocol_logic.h"

enum class XiaozhiInboundKind : uint8_t { TEXT = 1, BINARY = 2 };

class XiaozhiTransport
{
public:
    XiaozhiTransport();
    ~XiaozhiTransport();
    bool begin(const xiaozhi::ProvisionedWebsocket &config,
               const char *ca_cert, const char *device_id,
               const char *client_id, uint32_t generation,
               char *error, size_t error_size);
    void loop();
    void close();
    void purgeUplink();
    bool connected() const { return connected_.load(std::memory_order_acquire); }
    bool sendText(const char *text);
    bool queueAudio(const uint8_t *data, size_t size, uint32_t generation);
    bool receive(XiaozhiInboundKind *kind, uint8_t *data, size_t capacity,
                 size_t *size, uint32_t *generation);
    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }
    uint32_t droppedUplink() const { return dropped_uplink_.load(std::memory_order_relaxed); }
    uint32_t droppedDownlink() const { return dropped_downlink_.load(std::memory_order_relaxed); }
    size_t uplinkPending() const;
    size_t uplinkCapacity() const { return 8; }
    bool inFlight() const { return in_flight_started_ms_ != 0; }
    bool uplinkIdle() const { return uplinkPending() == 0 && in_flight_started_ms_ == 0; }
    size_t inboundPending() const;
    bool inboundIdle() const { return inboundPending() == 0; }
    bool audioQueueHasCapacity(uint32_t generation) const;
    bool setGeneration(uint32_t generation);
    bool setTurnGeneration(uint32_t generation);
    void detachTurn();
    uint32_t connectionEpoch() const { return connection_epoch_.load(std::memory_order_acquire); }
    uint32_t turnGeneration() const { return generation_.load(std::memory_order_acquire); }
    uint32_t framesSent() const { return frames_sent_.load(std::memory_order_relaxed); }
    uint32_t bytesSent() const { return bytes_sent_.load(std::memory_order_relaxed); }
    uint32_t lastAudioSentMs() const { return last_audio_sent_ms_.load(std::memory_order_acquire); }
    void resetAudioCounters()
    {
        frames_sent_.store(0, std::memory_order_relaxed);
        bytes_sent_.store(0, std::memory_order_relaxed);
        last_audio_sent_ms_.store(0, std::memory_order_relaxed);
    }

private:
    struct AudioPacket
    {
        uint32_t generation;
        uint16_t size;
        uint8_t data[xiaozhi::kMaxOpusPacketBytes + 16];
    };
    struct InboundMessage
    {
        XiaozhiInboundKind kind;
        uint32_t generation;
        size_t size;
        uint8_t data[1];
    };

    esp_websocket_client_handle_t websocket_;
    String uri_;
    String headers_;
    QueueHandle_t uplink_queue_;
    QueueHandle_t inbound_queue_;
    SemaphoreHandle_t rx_mutex_;
    uint8_t *fragment_storage_;
    xiaozhi::FragmentAssembler *fragment_assembler_;
    std::atomic<bool> connected_;
    std::atomic<uint32_t> connection_epoch_;
    std::atomic<uint32_t> generation_;
    std::atomic<uint32_t> dropped_uplink_;
    std::atomic<uint32_t> dropped_downlink_;
    std::atomic<uint32_t> frames_sent_;
    std::atomic<uint32_t> bytes_sent_;
    std::atomic<uint32_t> last_audio_sent_ms_;
    uint32_t in_flight_started_ms_ = 0;

    static void eventHandler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
    void onEvent(int32_t event_id, esp_websocket_event_data_t *event);
    bool enqueueInbound(XiaozhiInboundKind kind, const uint8_t *data, size_t size);
    void clearInbound();
};
