// Compile the firmware's xiaozhi_transport.cpp unchanged against these platform mocks.
#include "xiaozhi_transport.h"
#include "xiaozhi_session_logic.h"
#include "esp_heap_caps.h"

#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <vector>

struct MockQueue
{
    size_t capacity;
    size_t item_size;
    std::deque<std::vector<uint8_t>> items;
};
struct MockMutex { bool taken = false; };
struct MockSocket
{
    esp_event_handler_t callback = nullptr;
    void *context = nullptr;
    bool started = false;
    bool destroyed = false;
};

namespace mock
{
uint32_t now = 100;
unsigned queues = 0;
unsigned mutexes = 0;
unsigned live_sockets = 0;
bool fail_mutex = false;
unsigned fail_queue_at = 0;
unsigned create_queue_calls = 0;
bool fail_heap = false;
bool fail_init = false;
bool fail_register = false;
bool fail_start = false;
int send_result = -2; // -2 means full successful send; -1/0 means no data sent.
TickType_t send_delay = 0;
std::function<void()> after_send;
std::vector<std::vector<uint8_t>> sent;
std::vector<std::string> texts;
std::set<void *> heap;
std::vector<std::unique_ptr<MockSocket>> sockets;
MockSocket *latest = nullptr;

void emit(int32_t id, esp_websocket_event_data_t *event = nullptr)
{
    assert(latest && latest->callback);
    latest->callback(latest->context, "WEBSOCKET", id, event);
}
void frame(uint8_t opcode, const char *data, int size, int total, int offset = 0)
{
    esp_websocket_event_data_t event;
    event.op_code = opcode;
    event.data_ptr = data;
    event.data_len = size;
    event.payload_len = total;
    event.payload_offset = offset;
    emit(WEBSOCKET_EVENT_DATA, &event);
}
void reset_failures()
{
    fail_mutex = false;
    fail_queue_at = 0;
    create_queue_calls = 0;
    fail_heap = false;
    fail_init = false;
    fail_register = false;
    fail_start = false;
    send_result = -2;
    send_delay = 0;
    after_send = nullptr;
    sent.clear();
    texts.clear();
}
}

uint32_t millis() { return mock::now; }
QueueHandle_t xQueueCreate(UBaseType_t capacity, UBaseType_t item_size)
{
    if (++mock::create_queue_calls == mock::fail_queue_at) return nullptr;
    ++mock::queues;
    MockQueue *queue = new MockQueue;
    queue->capacity = capacity;
    queue->item_size = item_size;
    return queue;
}
void vQueueDelete(QueueHandle_t queue) { assert(queue); --mock::queues; delete queue; }
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t)
{
    if (queue->items.size() == queue->capacity) return pdFALSE;
    const uint8_t *bytes = static_cast<const uint8_t *>(item);
    queue->items.emplace_back(bytes, bytes + queue->item_size);
    return pdTRUE;
}
BaseType_t xQueuePeek(QueueHandle_t queue, void *item, TickType_t)
{
    if (queue->items.empty()) return pdFALSE;
    std::memcpy(item, queue->items.front().data(), queue->item_size);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    if (xQueuePeek(queue, item, wait) != pdTRUE) return pdFALSE;
    queue->items.pop_front();
    return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t queue) { queue->items.clear(); return pdTRUE; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    return static_cast<UBaseType_t>(queue->items.size());
}
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t queue)
{
    return static_cast<UBaseType_t>(queue->capacity - queue->items.size());
}
SemaphoreHandle_t xSemaphoreCreateMutex()
{
    if (mock::fail_mutex) return nullptr;
    ++mock::mutexes;
    return new MockMutex;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex) { assert(mutex); --mock::mutexes; delete mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t)
{
    assert(mutex && !mutex->taken);
    mutex->taken = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex && mutex->taken);
    mutex->taken = false;
    return pdTRUE;
}
void *heap_caps_malloc(size_t size, unsigned)
{
    if (mock::fail_heap) return nullptr;
    void *pointer = std::malloc(size);
    if (pointer) mock::heap.insert(pointer);
    return pointer;
}
void heap_caps_free(void *pointer)
{
    if (pointer) assert(mock::heap.erase(pointer) == 1);
    std::free(pointer);
}
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config)
{
    assert(config && config->cert_pem && config->cert_pem[0]);
    assert(config->disable_auto_reconnect);
    if (mock::fail_init) return nullptr;
    mock::sockets.emplace_back(new MockSocket);
    mock::latest = mock::sockets.back().get();
    ++mock::live_sockets;
    return mock::latest;
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client, int32_t,
                                       esp_event_handler_t callback, void *context)
{
    if (mock::fail_register) return ESP_FAIL;
    client->callback = callback;
    client->context = context;
    return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client)
{
    if (mock::fail_start) return ESP_FAIL;
    client->started = true;
    mock::emit(WEBSOCKET_EVENT_CONNECTED);
    return ESP_OK;
}
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client)
{
    if (client->callback)
    {
        client->callback(client->context, "WEBSOCKET", WEBSOCKET_EVENT_DISCONNECTED, nullptr);
        esp_websocket_event_data_t late;
        late.op_code = 0x81;
        late.data_ptr = "late";
        late.data_len = late.payload_len = 4;
        client->callback(client->context, "WEBSOCKET", WEBSOCKET_EVENT_DATA, &late);
    }
    client->started = false;
    return ESP_OK;
}
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client)
{
    assert(client && !client->destroyed);
    client->destroyed = true;
    --mock::live_sockets;
    return ESP_OK;
}
int esp_websocket_client_send_bin(esp_websocket_client_handle_t client, const char *data,
                                  int size, TickType_t timeout)
{
    assert(client && client->started && !client->destroyed);
    mock::now += std::min(mock::send_delay, timeout);
    const int sent = mock::send_result == -2 ? size : mock::send_result;
    if (sent > 0)
    {
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
        mock::sent.emplace_back(bytes, bytes + std::min(sent, size));
    }
    if (mock::after_send) mock::after_send();
    return sent;
}
int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data,
                                   int size, TickType_t)
{
    assert(client && client->started && !client->destroyed);
    mock::texts.emplace_back(data, size);
    return size;
}

static xiaozhi::ProvisionedWebsocket config()
{
    xiaozhi::ProvisionedWebsocket value = {};
    std::strcpy(value.url, "wss://example.test/xiaozhi/v1/");
    std::strcpy(value.token, "mock-device-token");
    value.version = 1;
    return value;
}
static bool begin(XiaozhiTransport &transport, uint32_t generation = 1)
{
    char error[160] = {};
    return transport.begin(config(), "mock-ca", "mock-device", "mock-client",
                           generation, error, sizeof(error));
}
static void test_mutex_failure()
{
    mock::fail_mutex = true;
    XiaozhiTransport transport;
    assert(!begin(transport));
    assert(!transport.connected());
    assert(mock::live_sockets == 0 && mock::queues == 0);
    mock::reset_failures();
    assert(begin(transport)); // Retry after memory recovers.
}
static void test_chunk_gap()
{
    XiaozhiTransport transport;
    assert(begin(transport));
    mock::frame(0x81, "abc", 3, 8, 0);
    mock::frame(0x81, "fgh", 3, 8, 5); // Missing offsets 3..4.
    assert(transport.inboundPending() == 0);
    assert(transport.droppedDownlink() == 1);
}

static void test_initialization_rollback()
{
    for (unsigned failure = 0; failure < 6; ++failure)
    {
        mock::reset_failures();
        XiaozhiTransport transport;
        if (failure < 2) mock::fail_queue_at = failure + 1;
        if (failure == 2) mock::fail_heap = true;
        if (failure == 3) mock::fail_init = true;
        if (failure == 4) mock::fail_register = true;
        if (failure == 5) mock::fail_start = true;
        assert(!begin(transport));
        assert(!transport.connected() && transport.generation() == 0);
        assert(mock::live_sockets == 0 && mock::queues == 0 && mock::heap.empty());
        mock::reset_failures();
        assert(begin(transport, failure + 1));
    }
}

static void expect_message(XiaozhiTransport &transport, const char *text,
                           uint32_t expected_generation)
{
    uint8_t output[256] = {};
    XiaozhiInboundKind kind;
    size_t size = 0;
    uint32_t generation = 0;
    assert(transport.receive(&kind, output, sizeof(output), &size, &generation));
    assert(kind == XiaozhiInboundKind::TEXT && generation == expected_generation);
    assert(size == std::strlen(text) && std::memcmp(output, text, size) == 0);
}

static void test_fragmentation_and_queue_limits()
{
    mock::reset_failures();
    XiaozhiTransport transport;
    assert(begin(transport, 5));
    mock::frame(0x81, "abc", 3, 8);
    mock::frame(0x89, "ping", 4, 4); // Control frames cannot interrupt SDK chunks.
    mock::frame(0x81, "def", 3, 8, 3);
    mock::frame(0x81, "gh", 2, 8, 6);
    expect_message(transport, "abcdefgh", 5);
    mock::frame(0x01, "abc", 3, 3); // Non-final WebSocket frame.
    mock::frame(0x00, "de", 2, 2);
    mock::frame(0x8a, "pong", 4, 4);
    mock::frame(0x80, "f", 1, 3);
    mock::frame(0x80, "gh", 2, 3, 1);
    expect_message(transport, "abcdefgh", 5);
    mock::frame(0x01, "empty-final", 11, 11);
    mock::frame(0x80, nullptr, 0, 0);
    expect_message(transport, "empty-final", 5);

    mock::frame(0x81, "abc", 3, 6);
    mock::frame(0x81, "bc", 2, 6, 1); // Repeated/overlapping bytes.
    assert(transport.inboundPending() == 0 && transport.droppedDownlink() == 1);
    mock::frame(0x81, "abc", 3, 6);
    mock::frame(0x82, "def", 3, 6, 3); // Changed opcode inside SDK frame.
    assert(transport.inboundPending() == 0 && transport.droppedDownlink() == 2);
    mock::frame(0x01, "unfinished", 10, 10);
    mock::frame(0x81, "new", 3, 3); // A second data message before FIN is invalid.
    assert(transport.inboundPending() == 0 && transport.droppedDownlink() == 3);
    mock::frame(0x80, "orphan", 6, 6);
    assert(transport.inboundPending() == 0 && transport.droppedDownlink() == 4);
    mock::frame(0x81, "x", 1, static_cast<int>(xiaozhi::kMaxJsonMessageBytes + 1));
    assert(transport.droppedDownlink() == 5);

    // Inbound OOM and a full queue must be observable and must free rejected messages.
    mock::fail_heap = true;
    mock::frame(0x81, "oom", 3, 3);
    mock::fail_heap = false;
    assert(transport.inboundPending() == 0 && transport.droppedDownlink() == 6);
    for (unsigned i = 0; i < 33; ++i) mock::frame(0x81, "queued", 6, 6);
    assert(transport.inboundPending() == 32 && transport.droppedDownlink() == 7);
    assert(mock::heap.size() == 33); // One fragment buffer + 32 messages.
    transport.close();
    assert(transport.inboundPending() == 0 && mock::heap.size() == 1);
    transport.close(); // Idempotent; late callbacks during SDK stop are discarded.
    assert(mock::live_sockets == 0);
}

static void test_large_backlog_and_send_failures()
{
    mock::reset_failures();
    XiaozhiTransport transport;
    assert(begin(transport, 7));
    uint8_t packet[4] = {};
    for (unsigned i = 0; i < 8; ++i)
    {
        packet[0] = static_cast<uint8_t>(i);
        assert(transport.queueAudio(packet, sizeof(packet), 7));
    }
    assert(!transport.queueAudio(packet, sizeof(packet), 7));
    assert(transport.droppedUplink() == 0); // Temporary full queue is backpressure.
    mock::send_result = 0;
    mock::send_delay = 30;
    transport.loop();
    assert(transport.uplinkPending() == 8 && transport.inFlight());
    assert(transport.framesSent() == 0 && transport.droppedUplink() == 0);
    mock::send_result = -2;
    unsigned produced = 8;
    while (produced < 100 || transport.uplinkPending())
    {
        while (produced < 100 && transport.audioQueueHasCapacity(7))
        {
            packet[0] = static_cast<uint8_t>(produced);
            assert(transport.queueAudio(packet, sizeof(packet), 7));
            ++produced;
        }
        const size_t before = mock::sent.size();
        const uint32_t start = mock::now;
        transport.loop();
        assert(mock::sent.size() - before <= 2);
        assert(mock::now - start <= 60); // At most two bounded SDK send waits.
    }
    assert(mock::sent.size() == 100 && transport.framesSent() == 100);
    for (unsigned i = 0; i < 100; ++i) assert(mock::sent[i][0] == i);
    assert(transport.droppedUplink() == 0 && transport.uplinkIdle());
    assert(xiaozhi::flush_ready_for_listen_stop(true, transport.uplinkPending(),
                                               transport.inFlight(), false));

    // An expired send is reported instead of silently marking the turn flushed.
    assert(transport.queueAudio(packet, sizeof(packet), 7));
    mock::send_result = -1;
    transport.loop();
    mock::now += 1500;
    transport.loop();
    assert(transport.droppedUplink() == 1 && transport.uplinkPending() == 0);
    assert(!xiaozhi::flush_ready_for_listen_stop(true, 0, false, true));
    // A partial SDK send corrupts WebSocket framing: close, never resend the prefix.
    assert(transport.queueAudio(packet, sizeof(packet), 7));
    mock::send_result = 2;
    transport.loop();
    assert(!transport.connected() && transport.generation() == 0);
    assert(transport.droppedUplink() == 2);
}

static void test_cancel_and_reconnect()
{
    mock::reset_failures();
    XiaozhiTransport transport;
    assert(begin(transport, 10));
    const uint8_t packet[] = {1, 2, 3, 4};
    for (unsigned i = 0; i < 8; ++i) assert(transport.queueAudio(packet, sizeof(packet), 10));
    mock::after_send = [&transport]() { transport.cancelTurn(10); };
    transport.loop();
    mock::after_send = nullptr;
    assert(mock::sent.size() == 1); // The in-flight write finishes; no next frame is sent.
    assert(!transport.queueAudio(packet, sizeof(packet), 10));
    assert(!transport.audioQueueHasCapacity(10));
    transport.loop();
    assert(mock::sent.size() == 1);
    mock::frame(0x81, "cancelled", 9, 9);
    assert(transport.inboundPending() == 0);
    transport.purgeUplink();
    assert(transport.sendText("{\"type\":\"abort\"}")); // Control path remains usable.
    transport.close();
    assert(!begin(transport, 10)); // A delayed start cannot resurrect the cancelled turn.
    assert(begin(transport, 11));
    transport.cancelTurn(10); // Old cancel does not close the next turn's audio gate.
    assert(transport.queueAudio(packet, sizeof(packet), 11));
    transport.loop();
    assert(transport.framesSent() == 1);
    mock::frame(0x81, "new", 3, 3);
    expect_message(transport, "new", 11);
    transport.detachTurn();
    assert(!transport.queueAudio(packet, sizeof(packet), 0));
    mock::frame(0x81, "late", 4, 4);
    assert(transport.inboundPending() == 0);
    assert(transport.setTurnGeneration(12));
    mock::emit(WEBSOCKET_EVENT_DISCONNECTED);
    assert(!transport.queueAudio(packet, sizeof(packet), 12));

    for (uint32_t generation = 13; generation < 113; ++generation)
    {
        assert(begin(transport, generation));
        assert(mock::queues == 2 && mock::live_sockets == 1 && mock::heap.size() == 1);
        mock::frame(0x81, "hello", 5, 5);
        assert(transport.queueAudio(packet, sizeof(packet), generation));
        transport.cancelTurn(generation);
        transport.close();
        assert(mock::live_sockets == 0 && transport.inboundPending() == 0);
        assert(mock::heap.size() == 1); // Reusable storage; no growth across 100 reconnects.
    }
}

static void test_idle_mcp_discovery()
{
    mock::reset_failures();
    XiaozhiTransport transport;
    assert(begin(transport, 20));
    const char *list = "{\"type\":\"mcp\",\"session_id\":\"test\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}}";
    const int length = static_cast<int>(std::strlen(list));
    // tools/list arriving with hello must survive handoff to idle.
    mock::frame(0x81, list, length, length);
    mock::frame(0x81, "{\"type\":\"tts\",\"state\":\"start\"}", 30, 30);
    transport.detachTurn();
    assert(transport.inboundPending() == 1);
    expect_message(transport, list, 0);
    // Idle control is delivered while late audio, TTS and actions are gated.
    mock::frame(0x81, list, length, length);
    expect_message(transport, list, 0);
    mock::frame(0x82, "audio", 5, 5);
    const char *call = "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":3,\"params\":{\"name\":\"self.music.play\"}}}";
    mock::frame(0x81, call, static_cast<int>(std::strlen(call)), static_cast<int>(std::strlen(call)));
    mock::frame(0x81, "{\"type\":\"tts\"}", 14, 14);
    mock::frame(0x81, "{invalid", 8, 8);
    assert(transport.inboundPending() == 0);
    // A PTT arriving before the next worker pass must not lose discovery.
    mock::frame(0x81, list, length, length);
    assert(transport.setTurnGeneration(21));
    expect_message(transport, list, 21);
    transport.cancelTurn(21);
    mock::frame(0x81, list, length, length);
    assert(transport.inboundPending() == 0);
    transport.close();
}

int main(int argc, char **argv)
{
    mock::reset_failures();
    const char *test = argc > 1 ? argv[1] : "all";
    if (std::strcmp(test, "mutex_failure") == 0 || std::strcmp(test, "all") == 0)
        test_mutex_failure();
    if (std::strcmp(test, "chunk_gap") == 0 || std::strcmp(test, "all") == 0)
        test_chunk_gap();
    if (std::strcmp(test, "all") == 0)
    {
        test_initialization_rollback();
        test_fragmentation_and_queue_limits();
        test_large_backlog_and_send_failures();
        test_cancel_and_reconnect();
        test_idle_mcp_discovery();
    }
    assert(mock::queues == 0 && mock::mutexes == 0 && mock::live_sockets == 0);
    assert(mock::heap.empty());
    std::puts("xiaozhi_transport_test PASS");
}
