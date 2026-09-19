#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "ai_voice_protocol.h"
#include "xiaozhi_protocol_logic.h"

struct McpAsyncJob
{
    uint32_t id;
    uint32_t generation;
    char session_id[96];
    xiaozhi::McpTool tool;
    AiMusicAction music_action;
};

struct McpAsyncResult
{
    uint32_t id;
    uint32_t generation;
    char session_id[96];
    bool is_error;
    char text[128];
};

enum class McpDispatchResult : uint8_t
{
    HANDLED_IMMEDIATE = 0,
    DISPATCH_ASYNC,
    ERROR_OR_REJECTED
};

class XiaozhiMcpServer
{
public:
    XiaozhiMcpServer();
    McpDispatchResult dispatch(JsonObjectConst payload, const char *session_id,
                               uint32_t generation,
                               String &outer_response, McpAsyncJob &async_job);
    McpDispatchResult dispatch(JsonObjectConst payload, const char *session_id,
                               String &outer_response, McpAsyncJob &async_job)
    {
        return dispatch(payload, session_id, 0, outer_response, async_job);
    }
    bool handle(JsonObjectConst payload, const char *session_id, String &outer_response);
    void resetSession();

    static void make_text_result(uint32_t id, const char *text, bool is_error,
                                 const char *session_id, String &out);
    static void make_error(uint32_t id, int code, const char *message,
                           const char *session_id, String &out);
    void remember(uint32_t id, const String &response);
    bool lookup(uint32_t id, String &response) const;

private:
    struct CachedResponse
    {
        bool valid;
        uint32_t id;
        String json;
    };
    CachedResponse cache_[8];
    size_t next_cache_;
};

