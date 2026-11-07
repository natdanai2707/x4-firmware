#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Thai has no spaces between words, so line breaks need word boundaries.
// ThaiSegmenter finds them with a word list (maximal matching: fewest unknown
// characters, then fewest words) and falls back to character-cluster
// boundaries for text the list does not cover. Pure C++, no storage access.
namespace ThaiSegmenter {

// Sorted word list in TIS-620 bytes (0xA1..0xFB = U+0E01..U+0E5B), as built by
// tools/thai/make_thai_dict.py:
//   "THD1" | u32 count | u32 offsets[count + 1] | word bytes
// Offsets index into the word bytes; word i is [offsets[i], offsets[i + 1]).
class Dictionary {
 public:
  // Borrow a loaded file image. False when the header does not match.
  bool attach(const uint8_t* data, size_t size);
  size_t wordCount() const { return count_; }

  // Calls onMatch(length) for every word that is a prefix of text[0..n), shortest first.
  template <typename F>
  void forEachPrefix(const uint8_t* text, size_t n, F&& onMatch) const;

 private:
  const uint8_t* word(uint32_t i) const { return bytes_ + offsets_[i]; }
  uint32_t length(uint32_t i) const { return offsets_[i + 1] - offsets_[i]; }

  const uint8_t* offsetsRaw_ = nullptr;
  const uint32_t* offsets_ = nullptr;
  const uint8_t* bytes_ = nullptr;
  uint32_t count_ = 0;
};

// TIS-620 byte for a Thai code point, or 0 when cp is not Thai.
inline uint8_t toTis(const uint32_t cp) {
  return cp >= 0x0E01 && cp <= 0x0E5B ? static_cast<uint8_t>(cp - 0x0E00 + 0xA0) : 0;
}

// True when a line may break between two adjacent Thai code points without
// splitting a character cluster (mark, following vowel, leading vowel, MAI YAMOK).
bool clusterBoundary(uint32_t left, uint32_t right);

// Byte offsets inside UTF-8 `text` where it may break: between Thai words,
// and where Thai meets other text. Empty when there is nothing to split.
// dict may be null (cluster boundaries only).
std::vector<size_t> breakByteOffsets(const std::string& text, const Dictionary* dict);

bool containsThai(const std::string& text);

// ---------------------------------------------------------------------------

template <typename F>
void Dictionary::forEachPrefix(const uint8_t* text, const size_t n, F&& onMatch) const {
  // [lo, hi) holds the words that start with text[0..k); narrow it one byte at a time.
  uint32_t lo = 0;
  uint32_t hi = count_;
  for (size_t k = 0; k < n && lo < hi; ++k) {
    const uint8_t c = text[k];
    // Byte k of word i, or -1 when the word ends at k (it sorts first).
    const auto key = [&](uint32_t i) { return length(i) > k ? static_cast<int>(word(i)[k]) : -1; };
    uint32_t a = lo, b = hi;
    while (a < b) {
      const uint32_t mid = a + (b - a) / 2;
      if (key(mid) < c) a = mid + 1; else b = mid;
    }
    uint32_t e = a, f = hi;
    while (e < f) {
      const uint32_t mid = e + (f - e) / 2;
      if (key(mid) <= c) e = mid + 1; else f = mid;
    }
    lo = a;
    hi = e;
    if (lo < hi && length(lo) == k + 1) onMatch(k + 1);
  }
}

}  // namespace ThaiSegmenter
