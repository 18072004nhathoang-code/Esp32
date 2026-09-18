#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

class XiaozhiMcpServer
{
public:
    XiaozhiMcpServer();
    bool handle(JsonObjectConst payload, const char *session_id, String &outer_response);
    void resetSession();

private:
    struct CachedResponse
    {
        uint32_t id;
        String json;
    };
    CachedResponse cache_[8];
    size_t next_cache_;

    bool lookup(uint32_t id, String &response) const;
    void remember(uint32_t id, const String &response);
};

