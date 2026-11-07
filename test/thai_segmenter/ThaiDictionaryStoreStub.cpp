// Host tests have no SD card: layout falls back to Thai cluster breaks.
#include "lib/Epub/Epub/ThaiDictionaryStore.h"

namespace ThaiDictionaryStore {
const ThaiSegmenter::Dictionary* get() { return nullptr; }
}  // namespace ThaiDictionaryStore
