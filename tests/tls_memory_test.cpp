#include "tls_memory.h"
#include "esp_heap_caps.h"
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <vector>

static void *(*allocate_tls)(size_t, size_t) = nullptr;
static void (*release_tls)(void *) = nullptr;
static bool fail_external = false;
static bool fail_internal = false;
static int setter_result = 0;
static unsigned live = 0;
static std::vector<unsigned> calls;

int mbedtls_platform_set_calloc_free(void *(*allocate)(size_t, size_t),
                                    void (*release)(void *))
{
    if (!setter_result) { allocate_tls = allocate; release_tls = release; }
    return setter_result;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    calls.push_back(caps);
    assert(caps & MALLOC_CAP_8BIT);
    if (((caps & MALLOC_CAP_SPIRAM) && fail_external) ||
        ((caps & MALLOC_CAP_INTERNAL) && fail_internal)) return nullptr;
    void *buffer = std::calloc(count, size);
    if (buffer) ++live;
    return buffer;
}
void heap_caps_free(void *buffer)
{
    if (buffer) { assert(live); --live; }
    std::free(buffer);
}
int main()
{
    setter_result = -1;
    assert(!tls_memory_init() && !allocate_tls && !release_tls);
    setter_result = 0;
    assert(tls_memory_init() && allocate_tls && release_tls);
    for (unsigned turn = 0; turn < 100; ++turn)
    {
        calls.clear();
        auto *buffer = static_cast<uint8_t *>(allocate_tls(2, 16384));
        assert(buffer && calls.size() == 1 && (calls[0] & MALLOC_CAP_SPIRAM));
        for (size_t i = 0; i < 32768; ++i) assert(buffer[i] == 0);
        release_tls(buffer);
        assert(live == 0);
    }
    calls.clear();
    fail_external = true;
    void *buffer = allocate_tls(3, 31);
    assert(buffer && calls.size() == 2 && (calls[1] & MALLOC_CAP_INTERNAL));
    release_tls(buffer);
    fail_internal = true;
    assert(!allocate_tls(1, 16384) && live == 0);
    calls.clear();
    assert(!allocate_tls(SIZE_MAX, 2));
    assert(!allocate_tls(0, 12));
    assert(!allocate_tls(12, 0));
    assert(calls.empty());
    release_tls(nullptr);
    std::puts("TLS memory implementation: PASS (PSRAM, fallback, OOM, overflow, zeroing, 100 cycles)");
}
