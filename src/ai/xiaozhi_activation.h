#pragma once

#include <Arduino.h>
#include "xiaozhi_protocol_logic.h"

struct XiaozhiProvisionResult
{
    bool configured;
    bool activation_required;
    char activation_code[32];
    char activation_message[160];
    uint32_t poll_after_ms;
    xiaozhi::ProvisionedWebsocket websocket;
};

bool xiaozhi_identity_init(char *device_id, size_t device_id_size,
                           char *client_id, size_t client_id_size);
bool xiaozhi_load_websocket_config(xiaozhi::ProvisionedWebsocket *config);
bool xiaozhi_provision_once(XiaozhiProvisionResult *result,
                            char *error, size_t error_size);

