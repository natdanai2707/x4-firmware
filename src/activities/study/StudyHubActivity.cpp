#include "StudyHubActivity.h"

#include <Memory.h>

#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr StudyModule modules[StudyHubActivity::MODULE_COUNT] = {StudyModule::COMPOUND, StudyModule::VOCAB,
                                                                 StudyModule::CHINESE};
constexpr UIIcon moduleIcons[StudyHubActivity::MODULE_COUNT] = {UIIcon::Book, UIIcon::Bookmark, UIIcon::Blocks};
}  // namespace

StudyHubActivity::StudyHubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("StudyHub", renderer, mappedInput) {
  for (int i = 0; i < MODULE_COUNT; i++) {
    fui::ListItem item;
    item.label = StudyPlaceholderActivity::titleFor(modules[i]);
    item.subtitle = StudyPlaceholderActivity::summaryFor(modules[i]);
    item.icon = listIconFor(moduleIcons[i], 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(i);
    rowItems_[i] = item;
  }
}

void StudyHubActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MODULE_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void StudyHubActivity::activateIndex(const int index) {
  if (index < 0 || index >= MODULE_COUNT) return;
  app.clearTapFlash();
  nav.selected = index;
  auto screen = makeUniqueNoThrow<StudyPlaceholderActivity>(renderer, mappedInput, modules[index]);
  if (!screen) {
    LOG_ERR("STUDY", "OOM: study module screen");
    return;
  }
  startActivityForResult(std::move(screen), nullptr);
}

void StudyHubActivity::onBackButton() { activityManager.goHome(HomeMenuItem::STUDY); }
