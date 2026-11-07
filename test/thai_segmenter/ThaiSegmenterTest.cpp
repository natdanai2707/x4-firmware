#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lib/Epub/Epub/ThaiSegmenter.h"

namespace {
std::vector<uint8_t> loadDict() {
  std::ifstream f(THAI_DICT_PATH, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// Split text at the offsets and join with "|".
std::string segmented(const std::string& text, const ThaiSegmenter::Dictionary* dict) {
  std::string out;
  size_t start = 0;
  for (const size_t cut : ThaiSegmenter::breakByteOffsets(text, dict)) {
    out += text.substr(start, cut - start) + "|";
    start = cut;
  }
  return out + text.substr(start);
}
}  // namespace

class ThaiSegmenterDict : public ::testing::Test {
 protected:
  void SetUp() override {
    bytes = loadDict();
    ASSERT_TRUE(dict.attach(bytes.data(), bytes.size()));
    ASSERT_GT(dict.wordCount(), 50000u);
  }
  std::vector<uint8_t> bytes;
  ThaiSegmenter::Dictionary dict;
};

TEST_F(ThaiSegmenterDict, SplitsSentenceIntoWords) {
  EXPECT_EQ(segmented("ฉันกินข้าว", &dict), "ฉัน|กินข้าว");  // "กินข้าว" is one dictionary entry
  EXPECT_EQ(segmented("ประเทศไทยมีประชากร", &dict), "ประเทศ|ไทย|มี|ประชากร");
  EXPECT_EQ(segmented("เศรษฐกิจโลกชะลอตัว", &dict), "เศรษฐกิจ|โลก|ชะลอตัว");
}

TEST_F(ThaiSegmenterDict, PrefersLongerKnownWords) {
  EXPECT_EQ(segmented("ความรู้รอบตัว", &dict).find("ความรู้"), 0u);
}

TEST_F(ThaiSegmenterDict, UnknownTextBreaksOnlyAtClusters) {
  for (const size_t cut : ThaiSegmenter::breakByteOffsets("เกฺงฺ", &dict)) {
    EXPECT_NE(cut, 3u);  // never right after the leading vowel SARA E
  }
}

TEST_F(ThaiSegmenterDict, MixedScripts) {
  EXPECT_EQ(segmented("AIกับเศรษฐกิจ", &dict), "AI|กับ|เศรษฐกิจ");
  EXPECT_EQ(segmented("ข้าว,", &dict), "ข้าว,");  // no break before punctuation
}

TEST(ThaiSegmenter, ClusterRules) {
  EXPECT_FALSE(ThaiSegmenter::clusterBoundary(0x0E01, 0x0E34));  // before SARA I
  EXPECT_FALSE(ThaiSegmenter::clusterBoundary(0x0E01, 0x0E32));  // before SARA AA
  EXPECT_FALSE(ThaiSegmenter::clusterBoundary(0x0E40, 0x0E01));  // after SARA E
  EXPECT_TRUE(ThaiSegmenter::clusterBoundary(0x0E32, 0x0E01));
}

TEST(ThaiSegmenter, NoDictionaryStillBreaksThai) {
  EXPECT_FALSE(ThaiSegmenter::breakByteOffsets("ภาษาไทย", nullptr).empty());
  EXPECT_TRUE(ThaiSegmenter::breakByteOffsets("English", nullptr).empty());
  EXPECT_TRUE(ThaiSegmenter::containsThai("abc ไทย"));
  EXPECT_FALSE(ThaiSegmenter::containsThai("abc"));
}
