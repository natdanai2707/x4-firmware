#include <gtest/gtest.h>

#include "lib/EpdFont/EpdFontData.h"
#include "lib/Utf8/Utf8.h"

// ============================================================================
// Anchor selection and placement for combining marks without GPOS tables.
//
// Hebrew niqqud whose identity depends on position must not use the default
// centre-and-raise heuristic: dagesh sits inside the letter body, the
// shin/sin dots sit over the letter's right/left arm, and holam hangs over
// the left corner (see PR #2541 review feedback).
// ============================================================================

using combiningMark::Anchor;
using combiningMark::anchorFor;
using combiningMark::anchorOver;
using combiningMark::anchorOverRotated90CW;
using combiningMark::raiseAboveBase;

TEST(AnchorFor, PositionSensitiveNiqqud) {
  EXPECT_EQ(anchorFor(0x05BC), Anchor::CenterNative);  // dagesh/mapiq
  EXPECT_EQ(anchorFor(0x05BA), Anchor::CenterNative);  // holam haser for vav
  EXPECT_EQ(anchorFor(0x05C1), Anchor::RightNative);   // shin dot
  EXPECT_EQ(anchorFor(0x05C2), Anchor::LeftNative);    // sin dot
  EXPECT_EQ(anchorFor(0x05B9), Anchor::LeftNative);    // holam
}

TEST(AnchorFor, EverythingElseKeepsCentreRaisedDefault) {
  EXPECT_EQ(anchorFor(0x05B0), Anchor::CenterRaised);  // Hebrew sheva
  EXPECT_EQ(anchorFor(0x05B7), Anchor::CenterRaised);  // Hebrew patach
  EXPECT_EQ(anchorFor(0x064E), Anchor::CenterRaised);  // Arabic fatha
  EXPECT_EQ(anchorFor(0x0651), Anchor::CenterRaised);  // Arabic shadda
  EXPECT_EQ(anchorFor(0x0301), Anchor::CenterRaised);  // combining acute
}

// Base glyph: cursor 100, left 1, width 12.  Mark: left 2, width 4.
TEST(AnchorOver, HorizontalPlacementPerAnchor) {
  // Centered: base bitmap spans [101, 113), centre 107; mark starts at 105.
  EXPECT_EQ(anchorOver(Anchor::CenterRaised, 100, 1, 12, 2, 4), 105 - 2);
  EXPECT_EQ(anchorOver(Anchor::CenterNative, 100, 1, 12, 2, 4), 105 - 2);
  // Left-aligned: mark bitmap starts at the base bitmap's left edge (101).
  EXPECT_EQ(anchorOver(Anchor::LeftNative, 100, 1, 12, 2, 4), 101 - 2);
  // Right-aligned: mark bitmap ends at the base bitmap's right edge (113).
  EXPECT_EQ(anchorOver(Anchor::RightNative, 100, 1, 12, 2, 4), 109 - 2);
}

// The rotated coordinate system inverts every left/width term, so the mark's
// offset from the base cursor must be the exact mirror of the unrotated one.
TEST(AnchorOverRotated90CW, MirrorsUnrotatedOffsets) {
  for (const Anchor anchor : {Anchor::CenterRaised, Anchor::CenterNative, Anchor::LeftNative, Anchor::RightNative}) {
    const int offset = anchorOver(anchor, 100, 1, 12, 2, 4) - 100;
    EXPECT_EQ(anchorOverRotated90CW(anchor, 100, 1, 12, 2, 4), 100 - offset);
  }
}

TEST(RaiseAboveBase, NativeAnchorsKeepFontDesignedHeight) {
  // A dagesh-like dot designed inside the letter body (top 8 of a base whose
  // top is 12) must NOT be hoisted above the letter.
  EXPECT_EQ(raiseAboveBase(Anchor::CenterNative, 8, 3, 12), 0);
  // Shin/sin dots overlapping the letter's top must not be pushed clear.
  EXPECT_EQ(raiseAboveBase(Anchor::RightNative, 13, 3, 12), 0);
  EXPECT_EQ(raiseAboveBase(Anchor::LeftNative, 13, 3, 12), 0);
}

TEST(RaiseAboveBase, CentreRaisedBehaviourUnchanged) {
  // Above-baseline mark colliding with the base: raised to restore a 1px gap.
  // gap = markTop - markHeight - baseTop = 10 - 3 - 12 = -5  ->  raise 6.
  EXPECT_EQ(raiseAboveBase(Anchor::CenterRaised, 10, 3, 12), 6);
  // Already clear of the base: no raise.
  EXPECT_EQ(raiseAboveBase(Anchor::CenterRaised, 16, 3, 12), 0);
  // Below-baseline mark (kasra, cedilla): stays at font-native position.
  EXPECT_EQ(raiseAboveBase(Anchor::CenterRaised, 2, 4, 12), 0);
}

// ============================================================================
// Thai mark placement (thaiMark::place). Pixel metrics modelled on Sarabun at
// ~20 px/em: base KO KAI advance 12, bitmap left 1 width 9 top 12 height 12.
// ============================================================================

namespace {
constexpr thaiMark::Box KO_KAI{1, 9, 12, 12};
constexpr thaiMark::Box PO_PLA{0, 12, 16, 16};    // ascender on the right
constexpr thaiMark::Box DO_CHADA{0, 11, 12, 18};  // descender to -6
constexpr thaiMark::Box SARA_I{-10, 9, 18, 5};    // bottom 13
constexpr thaiMark::Box MAI_EK{-3, 2, 18, 4};     // bottom 14, same level as SARA_I
constexpr thaiMark::Box SARA_U{-5, 4, -1, 5};     // under the baseline
constexpr int GAP = combiningMark::MIN_GAP_PX;
}  // namespace

TEST(ThaiMark, Kinds) {
  EXPECT_EQ(thaiMark::kindOf(0x0E34), thaiMark::Kind::Above);  // SARA I
  EXPECT_EQ(thaiMark::kindOf(0x0E31), thaiMark::Kind::Above);  // MAI HAN-AKAT
  EXPECT_EQ(thaiMark::kindOf(0x0E48), thaiMark::Kind::Tone);   // MAI EK
  EXPECT_EQ(thaiMark::kindOf(0x0E4C), thaiMark::Kind::Tone);   // THANTHAKHAT
  EXPECT_EQ(thaiMark::kindOf(0x0E38), thaiMark::Kind::Below);  // SARA U
  EXPECT_EQ(thaiMark::kindOf(0x0E01), thaiMark::Kind::None);   // KO KAI
  EXPECT_TRUE(utf8IsCombiningMark(0x0E49));
  EXPECT_FALSE(utf8IsCombiningMark(0x0E33));  // SARA AM advances
}

TEST(ThaiMark, PlainMarkKeepsFontPosition) {
  const auto p = thaiMark::place(thaiMark::Kind::Above, 0x0E01, 100, KO_KAI, 112, SARA_I, thaiMark::NO_STACK, GAP);
  EXPECT_EQ(p.x, 112);
  EXPECT_EQ(p.raise, 0);
}

TEST(ThaiMark, ToneStacksAboveUpperVowel) {
  const int vowelTop = SARA_I.top;  // 18
  const auto p = thaiMark::place(thaiMark::Kind::Tone, 0x0E01, 100, KO_KAI, 112, MAI_EK, vowelTop, GAP);
  EXPECT_EQ(p.x, 112);
  EXPECT_EQ(MAI_EK.top - MAI_EK.height + p.raise, vowelTop + GAP);  // bottom just clears the vowel
}

TEST(ThaiMark, ToneAloneStaysLow) {
  const auto p = thaiMark::place(thaiMark::Kind::Tone, 0x0E01, 100, KO_KAI, 112, MAI_EK, thaiMark::NO_STACK, GAP);
  EXPECT_EQ(p.raise, 0);  // bottom 14 already clears the base top 12
}

TEST(ThaiMark, UpperMarkMovesLeftOfTallAscender) {
  const auto p = thaiMark::place(thaiMark::Kind::Above, 0x0E1B, 100, PO_PLA, 113, SARA_I, thaiMark::NO_STACK, GAP);
  const int stem = PO_PLA.width / 6;
  EXPECT_EQ(p.x + SARA_I.left + SARA_I.width, 100 + PO_PLA.left + PO_PLA.width - stem - GAP);
  EXPECT_EQ(p.raise, 0);
}

TEST(ThaiMark, LowerVowelDropsBelowDescender) {
  const auto p = thaiMark::place(thaiMark::Kind::Below, 0x0E0E, 100, DO_CHADA, 112, SARA_U, thaiMark::NO_STACK, GAP);
  EXPECT_EQ(SARA_U.top + p.raise, (DO_CHADA.top - DO_CHADA.height) - GAP);
  const auto plain = thaiMark::place(thaiMark::Kind::Below, 0x0E01, 100, KO_KAI, 112, SARA_U, thaiMark::NO_STACK, GAP);
  EXPECT_EQ(plain.raise, 0);
}
