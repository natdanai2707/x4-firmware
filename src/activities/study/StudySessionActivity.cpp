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

// Thai (U+0E00..U+0E7F, UTF-8 lead E0 B8/B9) has no glyphs in the built-in
// fonts yet. Hide such fields instead of drawing replacement boxes; drop this
// once Thai rendering lands.
bool hasThai(const std::string& text) {
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    const auto b0 = static_cast<uint8_t>(text[i]);
    const auto b1 = static_cast<uint8_t>(text[i + 1]);
    if (b0 == 0xE0 && (b1 == 0xB8 || b1 == 0xB9)) return true;
  }
  return false;
}
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
  deck.reset();
  queue.clear();
  queue.shrink_to_fit();
}

void StudySessionActivity::loadCurrentCard() {
  if (!deck->readCard(queue[position], fields)) {
    fields.assign(1, "(card unreadable)");
  }
}

void StudySessionActivity::answer(const srs::Grade grade) {
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
        phase = Phase::Back;
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
  const int frontHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const auto lines = renderer.wrappedText(NOTOSANS_18_FONT_ID, fields[0].c_str(), width, 4);
  int y = top + (renderer.getScreenHeight() / 2 - top) - static_cast<int>(lines.size()) * frontHeight / 2;
  for (const auto& line : lines) {
    renderer.drawCenteredText(NOTOSANS_18_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += frontHeight;
  }
  y += frontHeight;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Tap to show answer");
}

void StudySessionActivity::renderBack(const int top, const int width) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = metrics.contentSidePadding;
  const int frontHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int bodyHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int zoneTop = renderer.getScreenHeight() - metrics.buttonHintsHeight - ANSWER_ZONE_GAP - ANSWER_ZONE_HEIGHT;

  int y = top;
  for (const auto& line : renderer.wrappedText(NOTOSANS_18_FONT_ID, fields[0].c_str(), width, 2)) {
    renderer.drawText(NOTOSANS_18_FONT_ID, x, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += frontHeight;
  }
  y += metrics.verticalSpacing;
  renderer.drawLine(x, y, x + width, y);
  y += metrics.verticalSpacing * 2;

  for (size_t f = 1; f < fields.size(); ++f) {
    if (hasThai(fields[f])) continue;
    // Field 2 (meaning) regular; later fields (examples, translations) italic.
    const auto style = f == 1 ? EpdFontFamily::REGULAR : EpdFontFamily::ITALIC;
    for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, fields[f].c_str(), width, 6)) {
      if (y + bodyHeight > zoneTop - metrics.verticalSpacing) break;
      renderer.drawText(UI_12_FONT_ID, x, y, line.c_str(), true, style);
      y += bodyHeight;
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
