#include "CompoundBriefActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t MAX_BRIEF_BYTES = 64 * 1024;
constexpr int TAB_BAR_HEIGHT = 44;
constexpr int GAP = 10;
constexpr const char* NEWS_TABS[] = {"Markets", "News", "Words"};
constexpr const char* IDEA_TABS[] = {"Idea", "Words"};
}  // namespace

CompoundBriefActivity::CompoundBriefActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("CompoundBrief", renderer, mappedInput), path(std::move(path)) {}

void CompoundBriefActivity::onEnter() {
  Activity::onEnter();
  std::string text;
  if (Storage.exists(path.c_str()) && Storage.readFileToString("COMPOUND", path.c_str(), MAX_BRIEF_BYTES, text)) {
    brief = CompoundBrief::parse(text);
  }
  loaded = !brief.empty();
  thaiFont = makeUniqueNoThrow<StudyFont>(renderer, StudyFont::Script::Thai);
  if (thaiFont && !thaiFont->load()) thaiFont.reset();
  layoutTab();
  requestUpdate();
}

void CompoundBriefActivity::onExit() {
  Activity::onExit();
  lines.clear();
  lines.shrink_to_fit();
  pageStarts.clear();
  thaiFont.reset();
  brief = CompoundBrief{};
}

const char* CompoundBriefActivity::tabName(const int i) const { return brief.isNews() ? NEWS_TABS[i] : IDEA_TABS[i]; }

int CompoundBriefActivity::fontFor(const std::string& text, const int latinFont) const {
  if (thaiFont && StudyFont::needsThai(text)) return thaiFont->bodyFontId();
  return latinFont;
}

int CompoundBriefActivity::bodyTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight + TAB_BAR_HEIGHT + GAP;
}

int CompoundBriefActivity::bodyBottom() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - m.buttonHintsHeight - GAP;
}

void CompoundBriefActivity::addText(const std::string& text, const int latinFont, const uint8_t style, const int indent,
                                    const int gapBefore, const bool rule) {
  if (text.empty()) return;
  const auto& m = UITheme::getInstance().getMetrics();
  const int fontId = fontFor(text, latinFont);
  // The Thai study font has one regular face.
  const uint8_t s = fontId == latinFont ? style : EpdFontFamily::REGULAR;
  const int width = renderer.getScreenWidth() - 2 * m.contentSidePadding - indent;
  bool first = true;
  for (auto& w : renderer.wrappedText(fontId, text.c_str(), width, 60, static_cast<EpdFontFamily::Style>(s))) {
    lines.push_back({fontId, s, indent, first ? gapBefore : 0, first && rule, std::move(w)});
    first = false;
  }
}

void CompoundBriefActivity::layoutTab() {
  lines.clear();
  pageStarts.clear();
  page = 0;
  if (!loaded) return;
  const auto B = EpdFontFamily::BOLD;
  const auto R = EpdFontFamily::REGULAR;
  const auto I = EpdFontFamily::ITALIC;
  const std::string name = tabName(tab);

  if (name == "Markets") {
    addText(brief.title, NOTOSANS_16_FONT_ID, B);
    addText(brief.titleTh, UI_12_FONT_ID, R);
    addText(brief.note, SMALL_FONT_ID, I, 0, GAP);
    for (const auto& mt : brief.metrics) {
      // label ........ value  change
      std::string row = mt.label + "   " + mt.value + "   " + mt.change;
      addText(row, UI_12_FONT_ID, B, 0, GAP, true);
      addText(mt.note, SMALL_FONT_ID, R, 12);
    }
  } else if (name == "News" || name == "Idea") {
    if (name == "Idea") {
      addText(brief.title, NOTOSANS_16_FONT_ID, B);
      addText(brief.titleTh, UI_12_FONT_ID, R);
      addText(brief.hook, UI_12_FONT_ID, I, 0, GAP);
      if (!brief.core.empty()) {
        addText("CORE IDEA", SMALL_FONT_ID, B, 0, GAP * 2, true);
        addText(brief.core, UI_12_FONT_ID, B);
        addText(brief.coreTh, UI_12_FONT_ID, R, 0, GAP / 2);
      }
    }
    for (size_t i = 0; i < brief.sections.size(); ++i) {
      const auto& s = brief.sections[i];
      const bool firstNews = name == "News" && i == 0;
      if (!s.framework.empty()) addText(s.framework, SMALL_FONT_ID, B, 0, GAP * 2, true);
      addText(s.headline, UI_12_FONT_ID, B, 0, s.framework.empty() && !firstNews ? GAP * 2 : GAP / 2,
              s.framework.empty() && !firstNews);
      addText(s.headlineTh, UI_12_FONT_ID, R);
      addText(s.en, UI_12_FONT_ID, R, 0, GAP / 2);
      addText(s.th, UI_12_FONT_ID, R, 0, GAP / 2);
      if (!s.impact.empty()) {
        addText("Why it matters: " + s.impact, UI_12_FONT_ID, I, 12, GAP / 2);
        addText(s.impactTh, UI_12_FONT_ID, R, 12);
      }
    }
    if (!brief.followups.empty()) {
      addText("THINK ABOUT", SMALL_FONT_ID, B, 0, GAP * 2, true);
      for (const auto& q : brief.followups) addText("- " + q, UI_12_FONT_ID, R, 0, GAP / 2);
    }
  } else {  // Words
    addText("Words from this brief. They are added to your IELTS deck on sync.", SMALL_FONT_ID, I);
    for (const auto& t : brief.terms) {
      addText(t.en, UI_12_FONT_ID, B, 0, GAP, true);
      addText(t.th, UI_12_FONT_ID, R);
      addText(t.explain, UI_12_FONT_ID, R, 0, GAP / 2);
      addText("\"" + t.example + "\"", UI_12_FONT_ID, I, 12, GAP / 2);
    }
  }
  addText("END OF " + std::string(name == "Words" ? "WORDS" : "BRIEF"), SMALL_FONT_ID, B, 0, GAP * 3, true);

  // Paginate.
  const int top = bodyTop();
  const int bottom = bodyBottom();
  int y = top;
  pageStarts.push_back(0);
  for (size_t i = 0; i < lines.size(); ++i) {
    const int h = renderer.getLineHeight(lines[i].fontId) + lines[i].gapBefore;
    if (y + h > bottom && i != pageStarts.back()) {
      pageStarts.push_back(i);
      y = top;
    }
    // A gap at the top of a page is wasted space.
    y += (i == pageStarts.back()) ? renderer.getLineHeight(lines[i].fontId) : h;
  }
}

void CompoundBriefActivity::setTab(const int t) {
  {
    RenderLock lock;
    tab = (t + tabCount()) % tabCount();
    layoutTab();
  }
  requestUpdate();
}

void CompoundBriefActivity::setPage(const int p) {
  const int count = static_cast<int>(pageStarts.size());
  if (p < 0 || p >= count || p == page) return;
  {
    RenderLock lock;
    page = p;
  }
  requestUpdate();
}

void CompoundBriefActivity::loop() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back)) {
    finish();
    return;
  }
  if (!loaded) {
    if (mappedInput.wasReleased(B::Confirm)) finish();
    return;
  }
  if (mappedInput.wasReleased(B::Right)) return setTab(tab + 1);
  if (mappedInput.wasReleased(B::Left)) return setTab(tab - 1);
  if (mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::PageForward) ||
      mappedInput.wasReleased(B::Confirm)) {
    // Confirm on the last page moves on to the next tab.
    if (page + 1 >= static_cast<int>(pageStarts.size()) && mappedInput.wasReleased(B::Confirm)) {
      if (tab + 1 < tabCount()) setTab(tab + 1);
      return;
    }
    return setPage(page + 1);
  }
  if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::PageBack)) return setPage(page - 1);

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    const auto& m = UITheme::getInstance().getMetrics();
    const int barTop = m.topPadding + m.headerHeight;
    if (y >= barTop && y < barTop + TAB_BAR_HEIGHT) {
      const int t = x * tabCount() / std::max(1, renderer.getScreenWidth());
      if (t != tab) setTab(t);
      return;
    }
    setPage(x >= renderer.getScreenWidth() / 2 ? page + 1 : page - 1);
  }
}

void CompoundBriefActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();

  std::string pages;
  if (loaded) pages = std::to_string(page + 1) + " / " + std::to_string(pageStarts.size());
  const std::string title = loaded ? (brief.isNews() ? "Markets · " : "Idea · ") + brief.date : "Compound";
  GUI.drawHeader(renderer, Rect{0, m.topPadding, w, m.headerHeight}, title.c_str(),
                 pages.empty() ? nullptr : pages.c_str());

  if (!loaded) {
    renderer.drawText(UI_12_FONT_ID, m.contentSidePadding, bodyTop(), "This brief could not be opened.", true,
                      EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, m.contentSidePadding, bodyTop() + 40, "Sync Compound again.");
  } else {
    // Tab bar: selected tab inverted.
    const int barTop = m.topPadding + m.headerHeight;
    const int n = tabCount();
    for (int i = 0; i < n; ++i) {
      const int x0 = i * w / n;
      const int x1 = (i + 1) * w / n;
      const bool sel = i == tab;
      if (sel) renderer.fillRect(x0, barTop, x1 - x0, TAB_BAR_HEIGHT, true);
      const int tw = renderer.getTextWidth(UI_12_FONT_ID, tabName(i), EpdFontFamily::BOLD);
      const int ty = barTop + (TAB_BAR_HEIGHT - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
      renderer.drawText(UI_12_FONT_ID, x0 + (x1 - x0 - tw) / 2, ty, tabName(i), !sel, EpdFontFamily::BOLD);
    }
    renderer.drawLine(0, barTop + TAB_BAR_HEIGHT - 1, w - 1, barTop + TAB_BAR_HEIGHT - 1, 2, true);

    const size_t from = pageStarts.empty() ? 0 : pageStarts[page];
    const size_t to = page + 1 < static_cast<int>(pageStarts.size()) ? pageStarts[page + 1] : lines.size();
    int y = bodyTop();
    for (size_t i = from; i < to; ++i) {
      const auto& l = lines[i];
      if (i != from) y += l.gapBefore;
      if (l.rule) renderer.drawLine(m.contentSidePadding, y - 3, w - m.contentSidePadding, y - 3);
      renderer.drawText(l.fontId, m.contentSidePadding + l.indent, y, l.text.c_str(), true,
                        static_cast<EpdFontFamily::Style>(l.style));
      y += renderer.getLineHeight(l.fontId);
    }
  }

  const char* next = loaded && tab + 1 < tabCount() ? tabName(tab + 1) : "";
  const char* prev = loaded && tab > 0 ? tabName(tab - 1) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), loaded ? "Next" : "OK", prev, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
