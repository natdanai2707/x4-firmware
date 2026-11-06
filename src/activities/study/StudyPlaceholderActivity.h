#pragma once
#include "activities/Activity.h"

enum class StudyModule { COMPOUND, VOCAB, CHINESE };

// Temporary screen for a study module that is not built yet. Back returns to
// the Study hub.
class StudyPlaceholderActivity final : public Activity {
  const StudyModule module;

 public:
  explicit StudyPlaceholderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StudyModule module)
      : Activity("StudyPlaceholder", renderer, mappedInput), module(module) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  static const char* titleFor(StudyModule module);
  static const char* summaryFor(StudyModule module);
};
