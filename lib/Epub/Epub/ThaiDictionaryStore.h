#pragma once

#include "ThaiSegmenter.h"

// The Thai word list for line breaking, loaded once from the SD card
// (/thai/words.dic) into PSRAM. Without the file or without PSRAM, get()
// returns null and Thai text breaks at character clusters instead.
namespace ThaiDictionaryStore {

constexpr const char* PATH = "/thai/words.dic";

const ThaiSegmenter::Dictionary* get();

}  // namespace ThaiDictionaryStore
