#include "StudySessionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <numeric>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* ANSWER_LABELS[3] = {"Again", "Good", "Easy"};
constexpr int ANSWER_ZONE_HEIGHT = 76;
constexpr int ANSWER_ZONE_GAP = 16;
constexpr int PAD = 16;
constexpr int ZONE_GAP = 10;
}  // namespace

StudySessionActivity::StudySessionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string deckName,
                                           std::string title)
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
    const auto& states = deck->states();
    today = std::accumulate(states.begin(), states.end(), today,
                            [](const srs::Day day, const srs::CardState& s) { return std::max(day, s.due); });
    if (today == 0) today = 1;
    LOG_INF("STUDY", "Clock not set, using day %u", static_cast<unsigned>(today));
  }

  queue = srs::planSession(deck->states(), today, srs::SessionLimits{}, deck->newIntroducedOn(today));
  plannedCount = queue.size();
  for (const uint16_t i : queue) (srs::isNew(deck->states()[i]) ? newCount : dueCount)++;
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
  graded[static_cast<int>(grade)]++;
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
  const int width = renderer.getScreenWidth() - 2 * PAD;
  const int col = (x - PAD) * 3 / std::max(1, width);
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

std::string StudySessionActivity::intervalText(const srs::Grade grade) const {
  if (position >= queue.size()) return "";
  const auto next = srs::review(deck->states()[queue[position]], grade, today);
  const int days = next.due > today ? next.due - today : 0;
  if (days == 0) return "Again today";
  return days == 1 ? "1 day" : std::to_string(days) + " days";
}

void StudySessionActivity::renderProgress(const int y, const int width) {
  const std::string count = std::to_string(std::min(position + 1, queue.size())) + " / " + std::to_string(queue.size());
  const int cw = renderer.getTextWidth(SMALL_FONT_ID, count.c_str(), EpdFontFamily::BOLD);
  const int barW = width - cw - 12;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int by = y + (smallH - 8) / 2;
  renderer.fillRoundedRect(PAD, by, barW, 8, 4, Color::LightGray);
  const int fill = queue.empty() ? 0 : static_cast<int>(barW * std::min(position, queue.size()) / queue.size());
  if (fill > 0) renderer.fillRoundedRect(PAD, by, std::max(8, fill), 8, 4, Color::Black);
  renderer.drawText(SMALL_FONT_ID, PAD + width - cw, y, count.c_str(), true, EpdFontFamily::BOLD);
}

void StudySessionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int width = pageWidth - 2 * PAD;

  std::string sub;
  if (phase == Phase::Front || phase == Phase::Back) {
    sub = std::to_string(dueCount) + " due, " + std::to_string(newCount) + " new";
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title.c_str(),
                 sub.empty() ? nullptr : sub.c_str());
  int top = metrics.topPadding + metrics.headerHeight + 12;
  if (phase == Phase::Front || phase == Phase::Back) {
    renderProgress(top, width);
    top += renderer.getLineHeight(SMALL_FONT_ID) + 20;
  }

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
      renderMessage(top, width, "Done for today", std::to_string(reviewed) + " cards reviewed. " + nextDueText());
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
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int zoneTop = renderer.getScreenHeight() - metrics.buttonHintsHeight - ANSWER_ZONE_GAP - ANSWER_ZONE_HEIGHT;
  const int fontId = fontFor(fields[0], true);
  // Built-in fonts look best bold; the study font has one regular face.
  const auto style = fontId == NOTOSANS_18_FONT_ID ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const auto lines = renderer.wrappedText(fontId, fields[0].c_str(), width, 4);
  const int blockH = smallH + 4 + 16 + static_cast<int>(lines.size()) * lineHeight;
  int y = top + (zoneTop - top - blockH) / 2;
  // NEW or REVIEW tag above the word.
  const bool isNew = position < queue.size() && srs::isNew(deck->states()[queue[position]]);
  const char* tag = isNew ? "NEW CARD" : "REVIEW";
  const int tw = renderer.getTextWidth(SMALL_FONT_ID, tag, EpdFontFamily::BOLD) + 14;
  const int tx = PAD + (width - tw) / 2;
  renderer.drawRoundedRect(tx, y, tw, smallH + 4, 2, 6, true);
  renderer.drawText(SMALL_FONT_ID, tx + 7, y + 2, tag, true, EpdFontFamily::BOLD);
  y += smallH + 4 + 16;
  for (const auto& line : lines) {
    renderer.drawCenteredText(fontId, y, line.c_str(), true, style);
    y += lineHeight;
  }
  renderer.fillRoundedRect(PAD, zoneTop, width, ANSWER_ZONE_HEIGHT, 14, Color::Black);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  const char* show = "Show answer";
  renderer.drawText(UI_12_FONT_ID, PAD + (width - renderer.getTextWidth(UI_12_FONT_ID, show, EpdFontFamily::BOLD)) / 2,
                    zoneTop + (ANSWER_ZONE_HEIGHT - bodyH) / 2, show, false, EpdFontFamily::BOLD);
}

void StudySessionActivity::renderBack(const int top, const int width) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = PAD;
  const int bodyHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int zoneTop = renderer.getScreenHeight() - metrics.buttonHintsHeight - ANSWER_ZONE_GAP - ANSWER_ZONE_HEIGHT;

  int y = top;
  const int frontId = fontFor(fields[0], true);
  const auto frontStyle = frontId == NOTOSANS_18_FONT_ID ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int frontHeight = renderer.getLineHeight(frontId);
  for (const auto& line : renderer.wrappedText(frontId, fields[0].c_str(), width, 2)) {
    renderer.drawText(frontId, x, y, line.c_str(), true, frontStyle);
    y += frontHeight;
  }
  y += 8;
  renderer.drawLine(x, y, x + width, y, 2, true);
  y += 16;

  for (size_t f = 1; f < fields.size(); ++f) {
    const int fontId = fontFor(fields[f], false);
    const int lineHeight = renderer.getLineHeight(fontId);
    // Meaning bold; later fields (examples) regular, italic where the font has it.
    const auto style = fontId != UI_12_FONT_ID ? EpdFontFamily::REGULAR
                       : f == 1                ? EpdFontFamily::BOLD
                                               : EpdFontFamily::ITALIC;
    for (const auto& line : renderer.wrappedText(fontId, fields[f].c_str(), width, 6)) {
      if (y + lineHeight > zoneTop - 12) break;
      renderer.drawText(fontId, x, y, line.c_str(), true, style);
      y += lineHeight;
    }
    y += 14;
  }

  // Grade buttons, matched by answerZoneAt(); each says when the card returns.
  const int zoneWidth = (width - 2 * ZONE_GAP) / 3;
  for (int i = 0; i < 3; ++i) {
    const int zx = x + i * (zoneWidth + ZONE_GAP);
    const bool primary = i == 1;
    if (primary) {
      renderer.fillRoundedRect(zx, zoneTop, zoneWidth, ANSWER_ZONE_HEIGHT, 14, Color::Black);
    } else {
      renderer.drawRoundedRect(zx, zoneTop, zoneWidth, ANSWER_ZONE_HEIGHT, 2, 14, true);
    }
    const std::string when = intervalText(static_cast<srs::Grade>(i));
    const int blockH = bodyHeight + smallH;
    const int ty = zoneTop + (ANSWER_ZONE_HEIGHT - blockH) / 2;
    const int textW = renderer.getTextWidth(UI_12_FONT_ID, ANSWER_LABELS[i], EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, zx + (zoneWidth - textW) / 2, ty, ANSWER_LABELS[i], !primary, EpdFontFamily::BOLD);
    const int ww = renderer.getTextWidth(SMALL_FONT_ID, when.c_str());
    renderer.drawText(SMALL_FONT_ID, zx + (zoneWidth - ww) / 2, ty + bodyHeight, when.c_str(), !primary);
  }
}

void StudySessionActivity::renderMessage(const int top, const int width, const char* heading, const std::string& body) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = PAD;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int bigH = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  int y = top + 28;
  // A check mark box, then the heading.
  renderer.fillRoundedRect(x, y, 64, 64, 18, Color::Black);
  renderer.drawLine(x + 18, y + 33, x + 28, y + 43, 4, false);
  renderer.drawLine(x + 28, y + 43, x + 47, y + 22, 4, false);
  y += 64 + 20;
  renderer.drawText(NOTOSANS_18_FONT_ID, x, y, heading, true, EpdFontFamily::BOLD);
  y += bigH + 10;
  for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, body.c_str(), width, 8)) {
    renderer.drawText(UI_12_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }
  if (phase == Phase::Done) {
    y += 18;
    const int boxH = smallH + bigH + 18;
    const int cw = width / 3;
    renderer.drawRoundedRect(x, y, width, boxH, 2, 12, true);
    for (int i = 0; i < 3; ++i) {
      const int cx = x + i * cw;
      if (i) renderer.drawLine(cx, y, cx, y + boxH - 1, 2, true);
      renderer.drawText(SMALL_FONT_ID, cx + 12, y + 6, ANSWER_LABELS[i]);
      renderer.drawText(NOTOSANS_18_FONT_ID, cx + 12, y + 8 + smallH, std::to_string(graded[i]).c_str(), true,
                        EpdFontFamily::BOLD);
    }
  }
  // Back to Home.
  const int by = renderer.getScreenHeight() - metrics.buttonHintsHeight - ANSWER_ZONE_GAP - 60;
  renderer.drawRoundedRect(x, by, width, 60, 2, 14, true);
  const char* back = "Back to Home";
  renderer.drawText(UI_12_FONT_ID, x + (width - renderer.getTextWidth(UI_12_FONT_ID, back, EpdFontFamily::BOLD)) / 2,
                    by + (60 - lineHeight) / 2, back, true, EpdFontFamily::BOLD);
}
