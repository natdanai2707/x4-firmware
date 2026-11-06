#include "StudyPlaceholderActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

const char* StudyPlaceholderActivity::titleFor(const StudyModule module) {
  switch (module) {
    case StudyModule::COMPOUND:
      return "Compound";
    case StudyModule::VOCAB:
      return "IELTS / TOEFL";
    case StudyModule::CHINESE:
      return "Chinese";
    default:
      return "";
  }
}

const char* StudyPlaceholderActivity::summaryFor(const StudyModule module) {
  switch (module) {
    case StudyModule::COMPOUND:
      return "Morning brief: Markets, Ideas, Words. Synced each morning.";
    case StudyModule::VOCAB:
      return "Daily words with examples and Thai meaning.";
    case StudyModule::CHINESE:
      return "Hanzi flashcards with pinyin and meaning.";
    default:
      return "";
  }
}

void StudyPlaceholderActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void StudyPlaceholderActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void StudyPlaceholderActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int x = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, titleFor(module));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, summaryFor(module), contentWidth, 6)) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawText(UI_10_FONT_ID, x, y, "Coming soon.", true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
