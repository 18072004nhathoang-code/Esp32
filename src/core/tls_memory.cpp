#include "tls_memory.h"

#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <stdint.h>

namespace
{
void *tls_calloc(size_t count, size_t size)
{
    if (!count || !size || count > SIZE_MAX / size) return nullptr;
    // The pinned SDK otherwise forces all TLS buffers/RSA scratch into SRAM,
    // even with megabytes of free PSRAM. Keep DMA/task stacks internal; TLS
    // allocations are byte-addressable and use the SDK-supported external RAM.
    void *buffer = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer)
        buffer = heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return buffer;
}

void tls_free(void *buffer) { heap_caps_free(buffer); }
}

bool tls_memory_init()
{
    return mbedtls_platform_set_calloc_free(tls_calloc, tls_free) == 0;
}
