#include "nvs_utils.h"

#include <nvs.h>

bool nvs_namespace_exists(const char *name)
{
    if (!name || name[0] == '\0') return false;

    nvs_handle_t handle = 0;
    const esp_err_t result = nvs_open(name, NVS_READONLY, &handle);
    if (result != ESP_OK) return false;
    nvs_close(handle);
    return true;
}
