#pragma once

#include <cstddef>
#include <cstdlib>

#include "Arduino.h"

constexpr unsigned MALLOC_CAP_DEFAULT = 1U << 0;
constexpr unsigned MALLOC_CAP_SPIRAM = 1U << 1;
constexpr unsigned MALLOC_CAP_INTERNAL = 1U << 2;
constexpr unsigned MALLOC_CAP_8BIT = 1U << 3;

inline size_t simulatorPsramBytes() {
#if defined(BOARD_HAS_PSRAM)
  return 8U * 1024U * 1024U;
#else
  return 0;
#endif
}

inline size_t heap_caps_get_free_size(const unsigned caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? simulatorPsramBytes() : ESP.getFreeHeap();
}

inline size_t heap_caps_get_largest_free_block(const unsigned caps) { return heap_caps_get_free_size(caps); }

inline size_t heap_caps_get_minimum_free_size(const unsigned caps) { return heap_caps_get_free_size(caps); }

inline size_t heap_caps_get_total_size(const unsigned caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? simulatorPsramBytes() : ESP.getHeapSize();
}

inline void* heap_caps_malloc(const size_t size, unsigned /*caps*/) { return std::malloc(size); }

inline void* heap_caps_realloc(void* ptr, const size_t size, unsigned /*caps*/) { return std::realloc(ptr, size); }

inline void heap_caps_free(void* ptr) { std::free(ptr); }
