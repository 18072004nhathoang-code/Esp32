#pragma once

#include <Arduino.h>
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
    bool connected() const { return connected_; }
    bool sendText(const char *text);
    bool queueAudio(const uint8_t *data, size_t size, uint32_t generation);
    bool receive(XiaozhiInboundKind *kind, uint8_t *data, size_t capacity,
                 size_t *size, uint32_t *generation);
    uint32_t generation() const { return generation_; }
    uint32_t droppedUplink() const { return dropped_uplink_; }
    uint32_t droppedDownlink() const { return dropped_downlink_; }

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
    uint8_t *fragment_storage_;
    xiaozhi::FragmentAssembler *fragment_assembler_;
    volatile bool connected_;
    uint32_t generation_;
    uint32_t dropped_uplink_;
    uint32_t dropped_downlink_;

    static void eventHandler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
    void onEvent(int32_t event_id, esp_websocket_event_data_t *event);
    bool enqueueInbound(XiaozhiInboundKind kind, const uint8_t *data, size_t size);
    void clearInbound();
};
