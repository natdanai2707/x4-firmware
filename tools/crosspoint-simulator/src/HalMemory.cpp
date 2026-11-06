#include "HalMemory.h"

#include "Arduino.h"
#include "esp_heap_caps.h"

namespace {
HalMemory::HeapStats readMockHeap() {
  return {ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getMinFreeHeap(),
          ESP.getMaxAllocHeap()};
}
} // namespace

// Internal RAM reports the configurable mock heap; PSRAM is present only for
// simulator profiles that declare BOARD_HAS_PSRAM.
HalMemory::HeapStats HalMemory::getDefaultHeap() { return readMockHeap(); }

HalMemory::HeapStats HalMemory::getInternalHeap() { return readMockHeap(); }

HalMemory::HeapStats HalMemory::getPsramHeap() {
  return {heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
          heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
          heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)};
}

void HalMemory::PsramDeleter::operator()(uint8_t *buffer) const {
  heap_caps_free(buffer);
}

HalMemory::PsramBuffer HalMemory::allocatePsram(const size_t bytes) {
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0)
    return {};
  return PsramBuffer(static_cast<uint8_t *>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
