#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "study/CompoundBrief.h"
#include "study/StudyFont.h"

// Native view of one Compound brief (/compound/<id>.cb) with tabs:
//   news:    Markets, News, Words
//   concept: Idea, Words
// Left/Right switch tab, Up/Down and page keys turn pages, Confirm goes to
// the next page. Touch: tap a tab, tap the right or left half to turn pages.
class CompoundBriefActivity final : public Activity {
 public:
  CompoundBriefActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Line {
    int fontId;
    uint8_t style;  // EpdFontFamily style
    int indent;
    int gapBefore;
    bool rule;  // thin line above
    std::string text;
  };

  void layoutTab();
  void addText(const std::string& text, int fontId, uint8_t style, int indent = 0, int gapBefore = 0,
               bool rule = false);
  int fontFor(const std::string& text, int latinFont) const;
  int tabCount() const { return brief.isNews() ? 3 : 2; }
  const char* tabName(int i) const;
  void setTab(int t);
  void setPage(int p);
  int bodyTop() const;
  int bodyBottom() const;

  const std::string path;
  CompoundBrief brief;
  std::unique_ptr<StudyFont> thaiFont;
  std::vector<Line> lines;
  std::vector<size_t> pageStarts;  // index into lines
  int tab = 0;
  int page = 0;
  bool loaded = false;
};
