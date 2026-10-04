#include "xiaozhi_mcp.h"
#include "xiaozhi_mcp_control.h"
#include "../src/camera/camera_service.h"
#include <cassert>
#include <cstdio>

static unsigned executions = 0;
static bool device_ack = false;
bool music_player_execute_ai_action(const AiMusicAction *, uint32_t, char *error, size_t size)
{
    ++executions;
    if (!device_ack) strlcpy(error, "SD unavailable", size);
    return device_ack;
}
bool ui_open_camera_app() { return false; }
bool camera_service_start() { return false; }
bool camera_service_stop(uint32_t) { return true; }
bool camera_service_is_available() { return false; }
bool camera_service_is_connected() { return false; }
CameraRuntimeState camera_service_get_runtime_state() { return CAM_STATE_NOT_CONFIGURED; }
const char *camera_service_get_model_name() { return "unavailable"; }
bool time_service_is_synced() { return false; }
bool time_service_format_clock(char *, size_t) { return false; }

int main()
{
    XiaozhiMcpServer server;
    DynamicJsonDocument request(2048), response(8192);
    String output;
    McpAsyncJob job = {};
    assert(!deserializeJson(request, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}"));
    assert(server.dispatch(request.as<JsonObjectConst>(), "session-a", 0, output, job) == McpDispatchResult::HANDLED_IMMEDIATE);
    assert(!deserializeJson(response, output.c_str()));
    assert(response["payload"]["result"]["capabilities"]["tools"].is<JsonObject>());
    assert(!deserializeJson(request, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{\"cursor\":\"\"}}"));
    assert(server.dispatch(request.as<JsonObjectConst>(), "session-a", 0, output, job) == McpDispatchResult::HANDLED_IMMEDIATE);
    assert(!deserializeJson(response, output.c_str()));
    JsonObjectConst result = response["payload"]["result"].as<JsonObjectConst>();
    assert(result["tools"].size() == 11 && !result.containsKey("nextCursor"));
    assert(strcmp(result["tools"][0]["name"] | "", "self.music.play") == 0);
    assert(strstr(result["tools"][0]["description"] | "", "arguments {}"));
    assert(!result["tools"][0]["inputSchema"].containsKey("required"));
    // Empty arguments explicitly select SD; neither a fake URL nor YouTube query.
    assert(!deserializeJson(request, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"self.music.play\",\"arguments\":{}}}"));
    assert(server.dispatch(request.as<JsonObjectConst>(), "session-a", 5, output, job) == McpDispatchResult::DISPATCH_ASYNC);
    assert(job.generation == 5 && job.music_action.type == AI_MUSIC_ACTION_PLAY);
    assert(!job.music_action.query[0] && !job.music_action.source_id[0]);
    assert(executions == 0); // Dispatch must not block the UI or claim ACK.
    assert(server.handle(request.as<JsonObjectConst>(), "session-a", output));
    assert(!deserializeJson(response, output.c_str()));
    assert(executions == 1 && response["payload"]["result"]["isError"].as<bool>());
    assert(server.handle(request.as<JsonObjectConst>(), "session-a", output));
    assert(executions == 1); // Replay cached result, don't repeat failed I/O.
    server.resetSession();
    device_ack = true;
    assert(server.handle(request.as<JsonObjectConst>(), "session-b", output));
    assert(!deserializeJson(response, output.c_str()));
    assert(executions == 2 && !response["payload"]["result"]["isError"].as<bool>());
    assert(strcmp(response["session_id"] | "", "session-b") == 0);
    assert(!deserializeJson(request, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"self.music.play\",\"arguments\":{\"url\":\"https://arbitrary.example/song.mp3\"}}}"));
    assert(server.dispatch(request.as<JsonObjectConst>(), "session-b", 6, output, job) == McpDispatchResult::HANDLED_IMMEDIATE);
    assert(!deserializeJson(response, output.c_str()) && response["payload"].containsKey("error"));
    assert(executions == 2);
    const char *control = "{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\"}}";
    assert(xiaozhi::is_idle_mcp_control(reinterpret_cast<const uint8_t *>(control), strlen(control)));
    assert(!xiaozhi::is_idle_mcp_control(nullptr, 0));
    std::puts("xiaozhi_mcp_test PASS (real dispatcher, discovery, SD command, ACK failure, dedup, URL rejection)");
}
