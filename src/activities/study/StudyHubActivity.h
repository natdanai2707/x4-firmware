#pragma once

#include "activities/UiListActivity.h"
#include "activities/study/StudyPlaceholderActivity.h"

// Study hub opened from the Home "Study" entry: Compound, IELTS/TOEFL vocab
// and Chinese flashcards. Back returns Home with Study selected.
class StudyHubActivity final : public UiListActivity {
 public:
  static constexpr int MODULE_COUNT = 3;

  explicit StudyHubActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

 private:
  int listCount() const override { return MODULE_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override { return "Study"; }

  // Static rows, built once in the constructor.
  freeink::ui::ListItem rowItems_[MODULE_COUNT]{};
};
