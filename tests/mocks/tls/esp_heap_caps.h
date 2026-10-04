#pragma once
#include <cstddef>
constexpr unsigned MALLOC_CAP_SPIRAM = 1;
constexpr unsigned MALLOC_CAP_8BIT = 2;
constexpr unsigned MALLOC_CAP_INTERNAL = 4;
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void heap_caps_free(void *pointer);
