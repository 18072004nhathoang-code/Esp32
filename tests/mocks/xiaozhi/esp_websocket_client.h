#pragma once
#include "esp_event.h"
#include "freertos/FreeRTOS.h"

enum
{
    WEBSOCKET_EVENT_ANY = -1,
    WEBSOCKET_EVENT_CONNECTED = 1,
    WEBSOCKET_EVENT_DISCONNECTED,
    WEBSOCKET_EVENT_ERROR,
    WEBSOCKET_EVENT_CLOSED,
    WEBSOCKET_EVENT_DATA
};
struct MockSocket;
using esp_websocket_client_handle_t = MockSocket *;
struct esp_websocket_event_data_t
{
    const char *data_ptr = nullptr;
    int data_len = 0;
    uint8_t op_code = 0;
    int payload_len = 0;
    int payload_offset = 0;
};
struct esp_websocket_client_config_t
{
    const char *uri = nullptr;
    const char *cert_pem = nullptr;
    const char *headers = nullptr;
    bool disable_auto_reconnect = false;
    int ping_interval_sec = 0;
    int pingpong_timeout_sec = 0;
    int task_prio = 0;
    int task_stack = 0;
    int buffer_size = 0;
    void *user_context = nullptr;
};
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client, int32_t id,
                                       esp_event_handler_t callback, void *context);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client);
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client);
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client);
int esp_websocket_client_send_bin(esp_websocket_client_handle_t client, const char *data,
                                  int size, TickType_t timeout);
int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data,
                                   int size, TickType_t timeout);
