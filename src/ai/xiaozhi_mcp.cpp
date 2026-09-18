#include "xiaozhi_mcp.h"

#include "../audio/music_player.h"
#include "../camera/camera_service.h"
#include "../os/time_service.h"
#include "../ui/ui_manager.h"
#include "ai_voice_protocol.h"
#include "xiaozhi_protocol_logic.h"

namespace
{
bool only_fields(JsonObjectConst object, const char *const *allowed, size_t allowed_count)
{
    for (JsonPairConst field : object)
    {
        bool known = false;
        for (size_t i = 0; i < allowed_count; ++i)
            if (strcmp(field.key().c_str(), allowed[i]) == 0) { known = true; break; }
        if (!known) return false;
    }
    return true;
}

void make_error(uint32_t id, int code, const char *message, const char *session_id, String &out)
{
    DynamicJsonDocument response(768);
    response["session_id"] = session_id ? session_id : "";
    response["type"] = "mcp";
    JsonObject payload = response.createNestedObject("payload");
    payload["jsonrpc"] = "2.0";
    payload["id"] = id;
    JsonObject error = payload.createNestedObject("error");
    error["code"] = code;
    error["message"] = message;
    out = "";
    serializeJson(response, out);
}

void make_text_result(uint32_t id, const char *text, bool is_error,
                      const char *session_id, String &out)
{
    DynamicJsonDocument response(1024);
    response["session_id"] = session_id ? session_id : "";
    response["type"] = "mcp";
    JsonObject payload = response.createNestedObject("payload");
    payload["jsonrpc"] = "2.0";
    payload["id"] = id;
    JsonObject result = payload.createNestedObject("result");
    JsonArray content = result.createNestedArray("content");
    JsonObject item = content.createNestedObject();
    item["type"] = "text";
    item["text"] = text ? text : "";
    result["isError"] = is_error;
    out = "";
    serializeJson(response, out);
}

bool no_arguments(JsonObjectConst args) { return args.isNull() || args.size() == 0; }
}

XiaozhiMcpServer::XiaozhiMcpServer() : next_cache_(0) {}

void XiaozhiMcpServer::resetSession()
{
    for (size_t i = 0; i < 8; ++i) { cache_[i].id = 0; cache_[i].json = ""; }
    next_cache_ = 0;
}

bool XiaozhiMcpServer::lookup(uint32_t id, String &response) const
{
    for (size_t i = 0; i < 8; ++i)
        if (id != 0 && cache_[i].id == id) { response = cache_[i].json; return true; }
    return false;
}

void XiaozhiMcpServer::remember(uint32_t id, const String &response)
{
    if (!id) return;
    cache_[next_cache_].id = id;
    cache_[next_cache_].json = response;
    next_cache_ = (next_cache_ + 1) % 8;
}

bool XiaozhiMcpServer::handle(JsonObjectConst request, const char *session_id,
                              String &outer_response)
{
    const char *request_fields[] = {"jsonrpc", "id", "method", "params"};
    if (request.isNull() || !only_fields(request, request_fields, 4) ||
        strcmp(request["jsonrpc"] | "", "2.0") != 0 || !request["id"].is<uint32_t>() ||
        !request["method"].is<const char *>()) return false;
    const uint32_t id = request["id"].as<uint32_t>();
    if (id == 0) return false;
    if (lookup(id, outer_response)) return true;
    const char *method = request["method"].as<const char *>();
    JsonObjectConst params = request["params"].as<JsonObjectConst>();

    if (strcmp(method, "initialize") == 0)
    {
        DynamicJsonDocument response(768);
        response["session_id"] = session_id ? session_id : "";
        response["type"] = "mcp";
        JsonObject payload = response.createNestedObject("payload");
        payload["jsonrpc"] = "2.0";
        payload["id"] = id;
        JsonObject result = payload.createNestedObject("result");
        result["protocolVersion"] = "2024-11-05";
        result.createNestedObject("capabilities").createNestedObject("tools");
        JsonObject info = result.createNestedObject("serverInfo");
        info["name"] = "esp32-mini-os";
        info["version"] = "1";
        outer_response = "";
        serializeJson(response, outer_response);
    }
    else if (strcmp(method, "tools/list") == 0)
    {
        const char *param_fields[] = {"cursor", "withUserTools"};
        if ((!params.isNull() && !only_fields(params, param_fields, 2)) ||
            (!params["cursor"].isNull() && strcmp(params["cursor"] | "", "") != 0) ||
            (params["withUserTools"] | false))
            make_error(id, -32602, "Invalid tools/list params", session_id, outer_response);
        else
        {
            DynamicJsonDocument response(6144);
            response["session_id"] = session_id ? session_id : "";
            response["type"] = "mcp";
            JsonObject payload = response.createNestedObject("payload");
            payload["jsonrpc"] = "2.0";
            payload["id"] = id;
            JsonObject result = payload.createNestedObject("result");
            JsonArray tools = result.createNestedArray("tools");
            const char *names[] = {"self.music.play", "self.music.pause", "self.music.resume",
                                   "self.music.stop", "self.music.set_volume", "self.camera.open",
                                   "self.camera.start", "self.camera.stop", "self.camera.refresh",
                                   "self.camera.get_status", "self.clock.get_time"};
            const char *descriptions[] = {"Phát nhạc từ SD hoặc source_id đã cấu hình",
                "Tạm dừng nhạc", "Tiếp tục nhạc", "Dừng nhạc", "Đặt âm lượng nhạc 0-100",
                "Mở ứng dụng Camera trên màn hình", "Khởi động dịch vụ Camera đã cấu hình",
                "Dừng dịch vụ Camera", "Kết nối lại Camera đã cấu hình",
                "Đọc trạng thái camera thật", "Đọc giờ hệ thống đã đồng bộ SNTP"};
            for (size_t i = 0; i < 11; ++i)
            {
                JsonObject tool = tools.createNestedObject();
                tool["name"] = names[i];
                tool["description"] = descriptions[i];
                JsonObject schema = tool.createNestedObject("inputSchema");
                schema["type"] = "object";
                JsonObject properties = schema.createNestedObject("properties");
                if (i == 0)
                {
                    JsonObject source = properties.createNestedObject("source_id");
                    source["type"] = "string";
                    source["maxLength"] = 31;
                }
                else if (i == 4)
                {
                    JsonObject value = properties.createNestedObject("value");
                    value["type"] = "integer";
                    value["minimum"] = 0;
                    value["maximum"] = 100;
                    schema.createNestedArray("required").add("value");
                }
                schema["additionalProperties"] = false;
            }
            result["nextCursor"] = "";
            outer_response = "";
            serializeJson(response, outer_response);
        }
    }
    else if (strcmp(method, "tools/call") == 0)
    {
        const char *call_fields[] = {"name", "arguments"};
        if (params.isNull() || !only_fields(params, call_fields, 2) ||
            !params["name"].is<const char *>() ||
            (!params["arguments"].isNull() && !params["arguments"].is<JsonObjectConst>()))
            make_error(id, -32602, "Invalid tools/call params", session_id, outer_response);
        else
        {
            const char *name = params["name"].as<const char *>();
            JsonObjectConst args = params["arguments"].as<JsonObjectConst>();
            AiMusicAction action = {};
            bool is_music = true;
            bool args_ok = true;
            const xiaozhi::McpTool tool_type = xiaozhi::mcp_tool_type(name);
            if (tool_type == xiaozhi::McpTool::MUSIC_PLAY)
            {
                action.type = AI_MUSIC_ACTION_PLAY;
                const char *fields[] = {"source_id"};
                args_ok = args.isNull() || only_fields(args, fields, 1);
                if (!args["source_id"].isNull())
                {
                    const char *source = args["source_id"].as<const char *>();
                    args_ok = args_ok && source && *source && strlen(source) < sizeof(action.source_id);
                    if (args_ok) strlcpy(action.source_id, source, sizeof(action.source_id));
                }
            }
            else if (tool_type == xiaozhi::McpTool::MUSIC_PAUSE) action.type = AI_MUSIC_ACTION_PAUSE;
            else if (tool_type == xiaozhi::McpTool::MUSIC_RESUME) action.type = AI_MUSIC_ACTION_RESUME;
            else if (tool_type == xiaozhi::McpTool::MUSIC_STOP) action.type = AI_MUSIC_ACTION_STOP;
            else if (tool_type == xiaozhi::McpTool::MUSIC_VOLUME)
            {
                action.type = AI_MUSIC_ACTION_VOLUME;
                const char *fields[] = {"value"};
                args_ok = !args.isNull() && only_fields(args, fields, 1) && args["value"].is<int>();
                const int value = args_ok ? args["value"].as<int>() : -1;
                args_ok = args_ok && xiaozhi::mcp_volume_valid(value);
                if (args_ok) action.volume = static_cast<uint8_t>(value);
            }
            else is_music = false;

            if (is_music)
            {
                if (action.type != AI_MUSIC_ACTION_PLAY && action.type != AI_MUSIC_ACTION_VOLUME)
                    args_ok = no_arguments(args);
                if (!args_ok || !ai_music_action_valid(action))
                    make_error(id, -32602, "Invalid music arguments", session_id, outer_response);
                else
                {
                    char action_error[128] = {};
                    const bool device_ack = music_player_execute_ai_action(&action, 2500,
                                                                            action_error, sizeof(action_error));
                    const bool executed = xiaozhi::mcp_result_success(true, device_ack);
                    make_text_result(id, executed ? "Lệnh nhạc đã được thiết bị ACK"
                                                  : action_error,
                                     !executed, session_id, outer_response);
                }
            }
            else if (tool_type == xiaozhi::McpTool::CAMERA_OPEN ||
                     tool_type == xiaozhi::McpTool::CAMERA_START ||
                     tool_type == xiaozhi::McpTool::CAMERA_STOP ||
                     tool_type == xiaozhi::McpTool::CAMERA_REFRESH)
            {
                if (!no_arguments(args))
                    make_error(id, -32602, "Camera tool has no arguments", session_id, outer_response);
                else
                {
                    bool ok = false;
                    if (tool_type == xiaozhi::McpTool::CAMERA_OPEN) ok = ui_open_camera_app();
                    else if (tool_type == xiaozhi::McpTool::CAMERA_START) ok = camera_service_start();
                    else if (tool_type == xiaozhi::McpTool::CAMERA_STOP) ok = camera_service_stop(2000);
                    else
                    {
                        const bool stopped = camera_service_stop(2000);
                        ok = stopped && camera_service_start();
                    }
                    make_text_result(id, ok ? "Camera đã ACK thao tác"
                                            : "Camera không thực hiện được thao tác",
                                     !ok, session_id, outer_response);
                }
            }
            else if (tool_type == xiaozhi::McpTool::CAMERA_STATUS)
            {
                if (!no_arguments(args)) make_error(id, -32602, "Camera tool has no arguments", session_id, outer_response);
                else
                {
                    char status[192];
                    snprintf(status, sizeof(status), "available=%s connected=%s state=%s model=%s",
                             camera_service_is_available() ? "true" : "false",
                             camera_service_is_connected() ? "true" : "false",
                             camera_runtime_state_to_string(camera_service_get_runtime_state()),
                             camera_service_get_model_name());
                    make_text_result(id, status, false, session_id, outer_response);
                }
            }
            else if (tool_type == xiaozhi::McpTool::CLOCK_TIME)
            {
                char clock[16] = {};
                const bool ok = no_arguments(args) && time_service_is_synced() &&
                                time_service_format_clock(clock, sizeof(clock));
                make_text_result(id, ok ? clock : "SNTP chưa đồng bộ", !ok,
                                 session_id, outer_response);
            }
            else make_error(id, -32601, "Tool not found", session_id, outer_response);
        }
    }
    else make_error(id, -32601, "Method not found", session_id, outer_response);

    remember(id, outer_response);
    return true;
}
