#include "ThaiDictionaryStore.h"

#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>

#include <atomic>

namespace ThaiDictionaryStore {

namespace {
// Upper bound for the file: the bundled list is about 0.7 MB.
constexpr size_t MAX_BYTES = 4 * 1024 * 1024;

HalMemory::PsramBuffer buffer;
ThaiSegmenter::Dictionary dictionary;
std::atomic<int> state{0};  // 0 not tried, 1 loaded, 2 unavailable

bool load() {
  if (!Storage.exists(PATH)) return false;
  HalFile file;
  if (!Storage.openFileForRead("THAI", PATH, file)) return false;
  const size_t size = file.size();
  if (size == 0 || size > MAX_BYTES) {
    file.close();
    return false;
  }
  // PSRAM only: on boards without it the list would compete with layout for DRAM.
  buffer = HalMemory::allocatePsram(size);
  if (!buffer) {
    file.close();
    LOG_INF("THAI", "No PSRAM for the word list; breaking at clusters");
    return false;
  }
  const int got = file.read(buffer.get(), size);
  file.close();
  if (got != static_cast<int>(size) || !dictionary.attach(buffer.get(), size)) {
    LOG_ERR("THAI", "Unreadable word list %s", PATH);
    buffer.reset();
    return false;
  }
  LOG_INF("THAI", "Loaded %u Thai words", static_cast<unsigned>(dictionary.wordCount()));
  return true;
}
}  // namespace

const ThaiSegmenter::Dictionary* get() {
  int s = state.load(std::memory_order_acquire);
  if (s == 0) {
    s = load() ? 1 : 2;
    state.store(s, std::memory_order_release);
  }
  return s == 1 ? &dictionary : nullptr;
}

}  // namespace ThaiDictionaryStore
