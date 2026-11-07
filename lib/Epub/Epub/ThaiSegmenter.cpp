#include "ThaiSegmenter.h"

#include <Utf8.h>

#include <cstring>
#include <limits>

namespace ThaiSegmenter {

namespace {
uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool isLeadingVowel(const uint32_t cp) { return cp >= 0x0E40 && cp <= 0x0E44; }  // SARA E..SARA AI MAIMALAI
bool isFollowingVowel(const uint32_t cp) {
  return cp == 0x0E30 || cp == 0x0E32 || cp == 0x0E33 || cp == 0x0E45;  // SARA A, AA, AM, LAKKHANGYAO
}

bool isAlnum(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp < 0x0E00);
}

struct Cost {
  uint32_t unknown = std::numeric_limits<uint32_t>::max();
  uint32_t words = 0;
  bool better(const Cost& o) const { return unknown != o.unknown ? unknown < o.unknown : words < o.words; }
};

// Boundaries inside one Thai run (positions in code points, excluding 0 and n).
void segmentRun(const std::vector<uint32_t>& cps, const size_t begin, const size_t end, const Dictionary* dict,
                std::vector<size_t>& out) {
  const size_t n = end - begin;
  if (n < 2) return;
  std::vector<uint8_t> tis(n);
  for (size_t i = 0; i < n; ++i) tis[i] = toTis(cps[begin + i]);
  std::vector<bool> legal(n + 1, false);
  legal[0] = legal[n] = true;
  for (size_t i = 1; i < n; ++i) legal[i] = clusterBoundary(cps[begin + i - 1], cps[begin + i]);

  std::vector<Cost> best(n + 1);
  std::vector<uint32_t> prev(n + 1, 0);
  best[0].unknown = 0;
  const auto relax = [&](size_t from, size_t to, uint32_t unknown) {
    Cost c{best[from].unknown + unknown, best[from].words + 1};
    if (c.better(best[to])) {
      best[to] = c;
      prev[to] = static_cast<uint32_t>(from);
    }
  };
  for (size_t i = 0; i < n; ++i) {
    if (!legal[i] || best[i].unknown == std::numeric_limits<uint32_t>::max()) continue;
    if (dict) {
      dict->forEachPrefix(tis.data() + i, n - i, [&](size_t len) {
        if (legal[i + len]) relax(i, i + len, 0);
      });
    }
    size_t j = i + 1;
    while (!legal[j]) ++j;
    relax(i, j, static_cast<uint32_t>(j - i));  // one unknown cluster
  }

  std::vector<size_t> cuts;
  for (size_t at = n; at > 0; at = prev[at]) cuts.push_back(at);
  for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
    if (*it < n) out.push_back(begin + *it);
  }
}
}  // namespace

bool Dictionary::attach(const uint8_t* data, const size_t size) {
  count_ = 0;
  if (!data || size < 12 || std::memcmp(data, "THD1", 4) != 0) return false;
  const uint32_t count = readU32(data + 4);
  const size_t offsetsBytes = (static_cast<size_t>(count) + 1) * 4;
  if (8 + offsetsBytes > size) return false;
  offsetsRaw_ = data + 8;
  // The file is little-endian like both targets; the offsets block starts 4-byte aligned.
  offsets_ = reinterpret_cast<const uint32_t*>(offsetsRaw_);
  bytes_ = data + 8 + offsetsBytes;
  if (offsets_[count] > size - 8 - offsetsBytes) return false;
  count_ = count;
  return true;
}

bool clusterBoundary(const uint32_t left, const uint32_t right) {
  if (utf8IsThaiMark(right)) return false;
  if (isFollowingVowel(right)) return false;
  if (right == 0x0E46 || right == 0x0E2F) return false;  // MAI YAMOK, PAIYANNOI stay with the word
  if (isLeadingVowel(left)) return false;
  return true;
}

bool containsThai(const std::string& text) {
  // Thai is U+0E00..U+0E7F: UTF-8 E0 B8 xx / E0 B9 xx.
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (static_cast<uint8_t>(text[i]) == 0xE0 &&
        (static_cast<uint8_t>(text[i + 1]) == 0xB8 || static_cast<uint8_t>(text[i + 1]) == 0xB9)) {
      return true;
    }
  }
  return false;
}

std::vector<size_t> breakByteOffsets(const std::string& text, const Dictionary* dict) {
  std::vector<uint32_t> cps;
  std::vector<size_t> byteAt;  // byte offset of each code point, plus the end
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = p;
  while (*p) {
    byteAt.push_back(static_cast<size_t>(p - start));
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    cps.push_back(cp);
  }
  byteAt.resize(cps.size());
  byteAt.push_back(text.size());

  std::vector<size_t> cuts;  // in code points
  size_t i = 0;
  while (i < cps.size()) {
    if (!utf8IsThai(cps[i])) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < cps.size() && utf8IsThai(cps[j])) ++j;
    // Script change between Thai and a Latin letter or digit; never next to
    // punctuation (no break before "," or after "(") or a mark.
    if (i > 0 && isAlnum(cps[i - 1]) && !utf8IsCombiningMark(cps[i])) cuts.push_back(i);
    segmentRun(cps, i, j, dict, cuts);
    if (j < cps.size() && isAlnum(cps[j])) cuts.push_back(j);
    i = j;
  }

  std::vector<size_t> offsets;
  offsets.reserve(cuts.size());
  for (const size_t c : cuts) {
    if (c > 0 && c < cps.size() && (offsets.empty() || byteAt[c] > offsets.back())) offsets.push_back(byteAt[c]);
  }
  return offsets;
}

}  // namespace ThaiSegmenter
