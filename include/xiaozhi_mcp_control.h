#pragma once

#include <ArduinoJson.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace xiaozhi
{
// Idle WSS still has to answer discovery. Never admit TTS or tool execution
// without an active voice generation; session authentication happens in the
// service before dispatch. Keep callback parsing bounded and allocation-free.
inline bool is_idle_mcp_control(JsonObjectConst root)
{
    const char *type = root["type"] | "";
    JsonObjectConst payload = root["payload"].as<JsonObjectConst>();
    if (strcmp(type, "mcp") != 0 || payload.isNull() ||
        strcmp(payload["jsonrpc"] | "", "2.0") != 0) return false;
    const char *method = payload["method"] | "";
    return strcmp(method, "initialize") == 0 || strcmp(method, "tools/list") == 0 ||
           strcmp(method, "notifications/initialized") == 0;
}

inline bool is_idle_mcp_control(const uint8_t *data, size_t size)
{
    if (!data || !size || size > 1024) return false;
    StaticJsonDocument<1024> document;
    if (deserializeJson(document, data, size, DeserializationOption::NestingLimit(6)))
        return false;
    return document.is<JsonObject>() && is_idle_mcp_control(document.as<JsonObjectConst>());
}
}
