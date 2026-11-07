#include "StudySessionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* ANSWER_LABELS[3] = {"Again", "Good", "Easy"};
constexpr int ANSWER_ZONE_HEIGHT = 72;
constexpr int ANSWER_ZONE_GAP = 10;
}  // namespace

StudySessionActivity::StudySessionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string deckName, std::string title)
    : Activity("StudySession", renderer, mappedInput),
      deck(makeUniqueNoThrow<StudyDeck>(std::move(deckName))),
      title(std::move(title)) {}

void StudySessionActivity::onEnter() {
  Activity::onEnter();
  if (!deck || !deck->load()) {
    phase = Phase::NoDeck;
    requestUpdate();
    return;
  }

  today = StudyDeck::today();
  if (today == 0) {
    // No clock yet: keep scheduling relative to the last study day.
    for (const auto& s : deck->states()) today = std::max(today, s.due);
    if (today == 0) today = 1;
    LOG_INF("STUDY", "Clock not set, using day %u", static_cast<unsigned>(today));
  }

  queue = srs::planSession(deck->states(), today, srs::SessionLimits{}, deck->newIntroducedOn(today));
  plannedCount = queue.size();
  if (queue.empty()) {
    phase = Phase::NothingDue;
  } else {
    phase = Phase::Front;
    loadCurrentCard();
  }
  requestUpdate();
}

void StudySessionActivity::onExit() {
  Activity::onExit();
  cjkFont.reset();
  thaiFont.reset();
  deck.reset();
  queue.clear();
  queue.shrink_to_fit();
}

void StudySessionActivity::loadCurrentCard() {
  if (!deck->readCard(queue[position], fields)) {
    fields.assign(1, "(card unreadable)");
  }
  const auto loadFont = [this](std::unique_ptr<StudyFont>& font, bool& tried, StudyFont::Script script,
                               bool (*needs)(const std::string&)) {
    if (tried || std::none_of(fields.begin(), fields.end(), needs)) return;
    tried = true;
    font = makeUniqueNoThrow<StudyFont>(renderer, script);
    if (font && !font->load()) font.reset();
  };
  loadFont(cjkFont, cjkTried, StudyFont::Script::Cjk, &StudyFont::needsFont);
  loadFont(thaiFont, thaiTried, StudyFont::Script::Thai, &StudyFont::needsThai);
}

int StudySessionActivity::fontFor(const std::string& text, const bool front) const {
  const StudyFont* font = nullptr;
  if (cjkFont && StudyFont::needsFont(text)) font = cjkFont.get();
  if (!font && thaiFont && StudyFont::needsThai(text)) font = thaiFont.get();
  if (font) return front ? font->frontFontId() : font->bodyFontId();
  return front ? NOTOSANS_18_FONT_ID : UI_12_FONT_ID;
}

void StudySessionActivity::answer(const srs::Grade grade) {
  RenderLock lock;  // the render task reads fields and phase
  const uint16_t index = queue[position];
  auto& state = deck->states()[index];
  const bool wasNew = srs::isNew(state);
  state = srs::review(state, grade, today);
  if (wasNew) deck->noteNewIntroduced(today);
  if (position < plannedCount) ++reviewed;  // count each planned card once
  if (grade == srs::Grade::Again) queue.push_back(index);
  deck->save();

  ++position;
  if (position >= queue.size()) {
    phase = Phase::Done;
  } else {
    phase = Phase::Front;
    loadCurrentCard();
  }
  requestUpdate();
}

int StudySessionActivity::answerZoneAt(const int x, const int y) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = renderer.getScreenHeight() - metrics.buttonHintsHeight - ANSWER_ZONE_GAP - ANSWER_ZONE_HEIGHT;
  if (y < top || y >= top + ANSWER_ZONE_HEIGHT) return -1;
  const int width = renderer.getScreenWidth() - 2 * metrics.contentSidePadding;
  const int col = (x - metrics.contentSidePadding) * 3 / std::max(1, width);
  return col >= 0 && col < 3 ? col : -1;
}

void StudySessionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int tapX = 0;
  int tapY = 0;
  const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);

  switch (phase) {
    case Phase::Front:
      if (tapped || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        {
          RenderLock lock;
          phase = Phase::Back;
        }
        requestUpdate();
      }
      break;
    case Phase::Back: {
      const int zone = tapped ? answerZoneAt(tapX, tapY) : -1;
      if (zone == 0 || mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        answer(srs::Grade::Again);
      } else if (zone == 1 || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        answer(srs::Grade::Good);
      } else if (zone == 2 || mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        answer(srs::Grade::Easy);
      }
      break;
    }
    case Phase::NoDeck:
    case Phase::NothingDue:
    case Phase::Done:
      if (tapped || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) finish();
      break;
  }
}

std::string StudySessionActivity::nextDueText() const {
  if (!deck) return "";
  srs::Day next = UINT16_MAX;
  size_t dueToday = 0;
  for (const auto& s : deck->states()) {
    if (srs::isNew(s)) continue;
    if (s.due <= today) {
      ++dueToday;
    } else {
      next = std::min(next, s.due);
    }
  }
  if (dueToday > 0) return std::to_string(dueToday) + " cards still due today.";
  if (next == UINT16_MAX) return "New cards unlock tomorrow.";
  const int days = next - today;
  return days == 1 ? "Next review: tomorrow." : "Next review: in " + std::to_string(days) + " days.";
}

void StudySessionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int width = pageWidth - 2 * metrics.contentSidePadding;

  std::string progress;
  if (phase == Phase::Front || phase == Phase::Back) {
    progress = std::to_string(position + 1) + " / " + std::to_string(queue.size());
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str(),
                 progress.empty() ? nullptr : progress.c_str());
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;

  const char* btn2 = "";
  const char* btn3 = "";
  const char* btn4 = "";
  switch (phase) {
    case Phase::NoDeck:
      renderMessage(top, width, "No deck yet",
                    "Copy a card file to the SD card at " + (deck ? deck->tsvPath() : std::string("/study/")) +
                        ". One card per line, fields separated by tabs.");
      btn2 = tr(STR_BACK);
      break;
    case Phase::NothingDue:
      renderMessage(top, width, "All done for today", nextDueText());
      btn2 = "OK";
      break;
    case Phase::Done:
      renderMessage(top, width, "Session done",
                    "Reviewed " + std::to_string(reviewed) + " cards. " + nextDueText());
      btn2 = "OK";
      break;
    case Phase::Front:
      renderFront(top, width);
      btn2 = "Show";
      break;
    case Phase::Back:
      renderBack(top, width);
      btn2 = ANSWER_LABELS[1];
      btn3 = ANSWER_LABELS[0];
      btn4 = ANSWER_LABELS[2];
      break;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2, btn3, btn4);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void StudySessionActivity::renderFront(const int top, const int width) {
  const int fontId = fontFor(fields[0], true);
  // Built-in fonts look best bold; the study font has one regular face.
  const auto style = fontId == NOTOSANS_18_FONT_ID ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int lineHeight = renderer.getLineHeight(fontId);
  const auto lines = renderer.wrappedText(fontId, fields[0].c_str(), width, 4);
  int y = top + (renderer.getScreenHeight() / 2 - top) - static_cast<int>(lines.size()) * lineHeight / 2;
  for (const auto& line : lines) {
    renderer.drawCenteredText(fontId, y, line.c_str(), true, style);
    y += lineHeight;
  }
  y += renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Tap to show answer");
}

void StudySessionActivity::renderBack(const int top, const int width) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int bodyHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int zoneTop = renderer.getScreenHeight() - metrics.buttonHintsHeight - ANSWER_ZONE_GAP - ANSWER_ZONE_HEIGHT;

  int y = top;
  const int frontId = fontFor(fields[0], true);
  const auto frontStyle = frontId == NOTOSANS_18_FONT_ID ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int frontHeight = renderer.getLineHeight(frontId);
  for (const auto& line : renderer.wrappedText(frontId, fields[0].c_str(), width, 2)) {
    renderer.drawText(frontId, x, y, line.c_str(), true, frontStyle);
    y += frontHeight;
  }
  y += metrics.verticalSpacing;
  renderer.drawLine(x, y, x + width, y);
  y += metrics.verticalSpacing * 2;

  for (size_t f = 1; f < fields.size(); ++f) {
    const int fontId = fontFor(fields[f], false);
    const int lineHeight = renderer.getLineHeight(fontId);
    // Meaning regular; later fields (examples) italic where the font has it.
    const auto style = f == 1 || fontId != UI_12_FONT_ID ? EpdFontFamily::REGULAR : EpdFontFamily::ITALIC;
    for (const auto& line : renderer.wrappedText(fontId, fields[f].c_str(), width, 6)) {
      if (y + lineHeight > zoneTop - metrics.verticalSpacing) break;
      renderer.drawText(fontId, x, y, line.c_str(), true, style);
      y += lineHeight;
    }
    y += metrics.verticalSpacing * 2;
  }

  // Touch targets, matched by answerZoneAt().
  const int zoneWidth = width / 3;
  for (int i = 0; i < 3; ++i) {
    const int zx = x + i * zoneWidth + 4;
    const int zw = zoneWidth - 8;
    renderer.drawRoundedRect(zx, zoneTop, zw, ANSWER_ZONE_HEIGHT, 2, 10, true);
    const int textW = renderer.getTextWidth(UI_12_FONT_ID, ANSWER_LABELS[i], EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, zx + (zw - textW) / 2, zoneTop + (ANSWER_ZONE_HEIGHT - bodyHeight) / 2,
                      ANSWER_LABELS[i], true, EpdFontFamily::BOLD);
  }
}

void StudySessionActivity::renderMessage(const int top, const int width, const char* heading,
                                         const std::string& body) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  int y = top + metrics.verticalSpacing * 4;
  renderer.drawText(UI_12_FONT_ID, x, y, heading, true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;
  for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, body.c_str(), width, 8)) {
    renderer.drawText(UI_12_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }
}
