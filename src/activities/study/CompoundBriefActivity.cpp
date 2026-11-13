#include "CompoundBriefActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t MAX_BRIEF_BYTES = 64 * 1024;
constexpr int GAP = 10;
constexpr int METRIC_COLS = 2;
constexpr const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string longDate(const std::string& iso) {
  if (iso.size() < 10) return iso;
  const int month = std::atoi(iso.substr(5, 2).c_str());
  if (month < 1 || month > 12) return iso;
  return std::string(MONTHS[month - 1]) + " " + std::to_string(std::atoi(iso.substr(8, 2).c_str())) + ", " +
         iso.substr(0, 4);
}
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
  layout();
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

int CompoundBriefActivity::fontFor(const std::string& text, const int latinFont) const {
  if (thaiFont && StudyFont::needsThai(text)) return thaiFont->bodyFontId();
  return latinFont;
}

int CompoundBriefActivity::bodyTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight + GAP;
}

int CompoundBriefActivity::bodyBottom() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - m.buttonHintsHeight - GAP;
}

void CompoundBriefActivity::addText(const std::string& text, const int latinFont, const uint8_t style, const int indent,
                                    const int gapBefore, const bool rule, const bool bar) {
  if (text.empty()) return;
  const auto& m = UITheme::getInstance().getMetrics();
  const int fontId = fontFor(text, latinFont);
  // The Thai study font has one regular face.
  const uint8_t s = fontId == latinFont ? style : EpdFontFamily::REGULAR;
  const int width = renderer.getScreenWidth() - 2 * m.contentSidePadding - indent;
  bool first = true;
  for (auto& w : renderer.wrappedText(fontId, text.c_str(), width, 60, static_cast<EpdFontFamily::Style>(s))) {
    lines.push_back({Kind::Text, fontId, s, indent, first ? gapBefore : 0, first && rule, bar, 0, 0, std::move(w)});
    first = false;
  }
}

void CompoundBriefActivity::layout() {
  lines.clear();
  pageStarts.clear();
  page = 0;
  if (!loaded) return;
  const auto B = EpdFontFamily::BOLD;
  const auto R = EpdFontFamily::REGULAR;
  const auto I = EpdFontFamily::ITALIC;
  const bool news = brief.isNews();

  // Title block.
  addText(std::string(news ? "MARKETS" : "IDEAS") + "  ·  " + longDate(brief.date) + "  ·  5 min read", SMALL_FONT_ID,
          B);
  addText(brief.title, NOTOSANS_18_FONT_ID, B, 0, 4);
  if (showThai) addText(brief.titleTh, UI_12_FONT_ID, R, 0, 4);
  // Like the app's "Show Thai translation" button; tap or Confirm toggles it.
  lines.push_back({Kind::ThaiToggle, UI_12_FONT_ID, B, 0, GAP, false, false, 44, 0, {}});

  if (news && !brief.metrics.empty()) {
    // Snapshot grid, two per row: label, value and change, note.
    const int rowH = renderer.getLineHeight(SMALL_FONT_ID) * 2 + renderer.getLineHeight(UI_12_FONT_ID) + 12;
    for (size_t i = 0; i < brief.metrics.size(); i += METRIC_COLS) {
      lines.push_back(
          {Kind::Metrics, UI_12_FONT_ID, R, 0, i == 0 ? GAP * 2 : 0, false, false, rowH, static_cast<int>(i), {}});
    }
    addText(brief.note, SMALL_FONT_ID, I, 0, 6);
  }
  if (!news) {
    addText(brief.hook, UI_12_FONT_ID, I, 0, GAP);
    if (!brief.core.empty()) {
      addText("CORE IDEA", SMALL_FONT_ID, B, 16, GAP * 2, false, true);
      addText(brief.core, UI_12_FONT_ID, B, 16, 4, false, true);
      if (showThai) addText(brief.coreTh, UI_12_FONT_ID, R, 16, 6, false, true);
    }
  }

  for (const auto& s : brief.sections) {
    if (!s.framework.empty()) {
      addText(s.framework.c_str(), SMALL_FONT_ID, B, 0, GAP * 3, true);
      addText(s.headline, UI_12_FONT_ID, B, 0, 4);
    } else {
      addText(s.headline, UI_12_FONT_ID, B, 0, GAP * 3, true);
    }
    if (showThai) addText(s.headlineTh, UI_12_FONT_ID, R, 0, 2);
    addText(s.en, UI_12_FONT_ID, R, 0, 6);
    if (showThai) addText(s.th, UI_12_FONT_ID, R, 16, 6, false, true);
    if (!s.impact.empty()) {
      addText("Why it matters here: " + s.impact, UI_12_FONT_ID, I, 16, GAP, false, true);
      if (showThai) addText(s.impactTh, UI_12_FONT_ID, R, 16, 4, false, true);
    }
  }

  if (!brief.terms.empty()) {
    addText("KEY TERMS", SMALL_FONT_ID, B, 0, GAP * 3, true);
    for (const auto& t : brief.terms) {
      addText(t.en + "   " + t.th, UI_12_FONT_ID, B, 0, GAP);
      addText(t.explain, UI_12_FONT_ID, R, 0, 2);
    }
  }
  if (!brief.followups.empty()) {
    addText("THINK ABOUT", SMALL_FONT_ID, B, 0, GAP * 3, true);
    for (const auto& q : brief.followups) addText("- " + q, UI_12_FONT_ID, R, 0, 6);
  }
  addText("END OF BRIEF", SMALL_FONT_ID, B, 0, GAP * 3, true);

  // Paginate. A gap at the top of a page is dropped.
  const int top = bodyTop();
  const int bottom = bodyBottom();
  const auto heightOf = [&](const Line& l) {
    return l.kind == Kind::Text ? renderer.getLineHeight(l.fontId) : l.height;
  };
  int y = top;
  pageStarts.push_back(0);
  for (size_t i = 0; i < lines.size(); ++i) {
    const bool pageStart = i == pageStarts.back();
    const int h = heightOf(lines[i]) + (pageStart ? 0 : lines[i].gapBefore);
    if (y + h > bottom && !pageStart) {
      pageStarts.push_back(i);
      y = top + heightOf(lines[i]);
      continue;
    }
    y += h;
  }
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
  if (mappedInput.wasReleased(B::Confirm)) {
    // Show or hide Thai, staying near the same place in the article.
    const size_t anchor = pageStarts.empty() ? 0 : pageStarts[page];
    const size_t total = std::max<size_t>(1, lines.size());
    {
      RenderLock lock;
      showThai = !showThai;
      layout();
      const size_t target = anchor * lines.size() / total;
      for (size_t p = 0; p < pageStarts.size(); ++p) {
        if (pageStarts[p] <= target) page = static_cast<int>(p);
      }
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::Right) ||
      mappedInput.wasReleased(B::PageForward)) {
    return setPage(page + 1);
  }
  if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::Left) || mappedInput.wasReleased(B::PageBack)) {
    return setPage(page - 1);
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (toggleTop >= 0 && y >= toggleTop && y < toggleBottom) {
      {
        RenderLock lock;
        showThai = !showThai;
        layout();
      }
      requestUpdate();
      return;
    }
    setPage(x >= renderer.getScreenWidth() / 2 ? page + 1 : page - 1);
  }
}

void CompoundBriefActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;

  std::string pages;
  if (loaded) pages = std::to_string(page + 1) + " / " + std::to_string(pageStarts.size());
  const char* title = loaded ? (brief.isNews() ? "Markets" : "Ideas") : "Compound";
  GUI.drawHeader(renderer, Rect{0, m.topPadding, w, m.headerHeight}, title, pages.empty() ? nullptr : pages.c_str());

  if (!loaded) {
    renderer.drawText(UI_12_FONT_ID, pad, bodyTop(), "This brief could not be opened.", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, pad, bodyTop() + 40, "Sync Compound again.");
  } else {
    const size_t from = pageStarts.empty() ? 0 : pageStarts[page];
    const size_t to = page + 1 < static_cast<int>(pageStarts.size()) ? pageStarts[page + 1] : lines.size();
    int y = bodyTop();
    toggleTop = toggleBottom = -1;
    const int cellW = (w - 2 * pad) / METRIC_COLS;
    for (size_t i = from; i < to; ++i) {
      const auto& l = lines[i];
      if (i != from) y += l.gapBefore;
      if (l.kind == Kind::ThaiToggle) {
        const char* label = showThai ? "Hide Thai translation" : "Show Thai translation";
        const int bw = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD) + 28;
        renderer.drawRoundedRect(pad, y, bw, l.height, 1, 4, true);
        renderer.drawText(UI_12_FONT_ID, pad + 14, y + (l.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2, label,
                          true, EpdFontFamily::BOLD);
        toggleTop = y;
        toggleBottom = y + l.height;
        y += l.height;
        continue;
      }
      if (l.kind == Kind::Metrics) {
        // One row of the snapshot grid.
        for (int c = 0; c < METRIC_COLS; ++c) {
          const size_t mi = static_cast<size_t>(l.metric + c);
          const int cx = pad + c * cellW;
          renderer.drawRect(cx, y, cellW, l.height);
          if (mi >= brief.metrics.size()) continue;
          const auto& mt = brief.metrics[mi];
          int ty = y + 4;
          renderer.drawText(SMALL_FONT_ID, cx + 10, ty, mt.label.c_str());
          ty += renderer.getLineHeight(SMALL_FONT_ID);
          // Value bold, change after it in the small font; the change takes the
          // note's line when the cell is too narrow for both.
          renderer.drawText(UI_12_FONT_ID, cx + 10, ty, mt.value.c_str(), true, EpdFontFamily::BOLD);
          const int vw = renderer.getTextWidth(UI_12_FONT_ID, mt.value.c_str(), EpdFontFamily::BOLD);
          const int chw = renderer.getTextWidth(SMALL_FONT_ID, mt.change.c_str(), EpdFontFamily::BOLD);
          const bool inline_ = 10 + vw + 8 + chw + 6 <= cellW;
          if (inline_) {
            renderer.drawText(SMALL_FONT_ID, cx + 10 + vw + 8,
                              ty + renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(SMALL_FONT_ID) - 2,
                              mt.change.c_str(), true, EpdFontFamily::BOLD);
          }
          ty += renderer.getLineHeight(UI_12_FONT_ID);
          if (!inline_) {
            renderer.drawText(SMALL_FONT_ID, cx + 10, ty, mt.change.c_str(), true, EpdFontFamily::BOLD);
          } else if (!mt.note.empty()) {
            const auto note = renderer.wrappedText(SMALL_FONT_ID, mt.note.c_str(), cellW - 20, 1);
            if (!note.empty()) renderer.drawText(SMALL_FONT_ID, cx + 10, ty, note[0].c_str());
          }
        }
        y += l.height;
        continue;
      }
      const int lh = renderer.getLineHeight(l.fontId);
      if (l.rule) renderer.drawLine(pad, y - 6, w - pad - 1, y - 6, 2, true);
      if (l.bar) renderer.fillRect(pad, y, 3, lh + (i + 1 < to && lines[i + 1].bar ? lines[i + 1].gapBefore : 0), true);
      renderer.drawText(l.fontId, pad + l.indent, y, l.text.c_str(), true, static_cast<EpdFontFamily::Style>(l.style));
      y += lh;
    }
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), loaded ? (showThai ? "Hide Thai" : "Show Thai") : "OK", "Prev", "Next");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
