#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "study/CompoundBrief.h"
#include "study/StudyFont.h"

// One Compound brief as a single article, laid out like the phone app's
// reader: title, market snapshot or core idea, sections, key terms and
// follow-up questions. Thai translations are hidden until shown, as in the
// app. Down/Right/Next turn pages, Up/Left go back, Confirm shows or hides
// Thai. Touch: right half next page, left half previous page.
class CompoundBriefActivity final : public Activity {
 public:
  CompoundBriefActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Kind : uint8_t { Text, Metrics, ThaiToggle };
  struct Line {
    Kind kind;
    int fontId;
    uint8_t style;  // EpdFontFamily style
    int indent;
    int gapBefore;
    bool rule;   // thin line above
    bool bar;    // accent bar on the left, for core idea, impact and examples
    int height;  // Metrics rows only
    int metric;  // first metric index of a Metrics row
    std::string text;
  };

  void layout();
  void addText(const std::string& text, int fontId, uint8_t style, int indent = 0, int gapBefore = 0, bool rule = false,
               bool bar = false);
  int fontFor(const std::string& text, int latinFont) const;
  void setPage(int p);
  int bodyTop() const;
  int bodyBottom() const;

  const std::string path;
  CompoundBrief brief;
  std::unique_ptr<StudyFont> thaiFont;
  std::vector<Line> lines;
  std::vector<size_t> pageStarts;
  int page = 0;
  bool showThai = false;
  bool loaded = false;
  int toggleTop = -1;  // Thai button band on the current page, -1 when absent
  int toggleBottom = -1;
};
