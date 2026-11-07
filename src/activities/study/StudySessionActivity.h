#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "study/StudyDeck.h"
#include "study/StudyFont.h"

// One review session over a StudyDeck: due cards, then new cards. Front, tap
// to show the back, then grade Again / Good / Easy. Progress is saved after
// every answer, so leaving midway loses nothing.
class StudySessionActivity final : public Activity {
 public:
  StudySessionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string deckName,
                       std::string title);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase { NoDeck, NothingDue, Front, Back, Done };

  void loadCurrentCard();
  void answer(srs::Grade grade);
  int answerZoneAt(int x, int y) const;  // 0 Again, 1 Good, 2 Easy, -1 none
  void renderFront(int top, int width);
  void renderBack(int top, int width);
  void renderMessage(int top, int width, const char* title, const std::string& body);
  std::string nextDueText() const;
  // Font for a card field: the study font when the text needs it, else built-in.
  int fontFor(const std::string& text, bool front) const;

  std::unique_ptr<StudyDeck> deck;
  const std::string title;
  Phase phase = Phase::Front;
  srs::Day today = 0;
  std::vector<uint16_t> queue;  // card indexes; Again re-appends
  size_t position = 0;
  size_t plannedCount = 0;  // size of the plan before any Again
  uint16_t reviewed = 0;
  std::vector<std::string> fields;
  // Loaded on the first card that needs them.
  std::unique_ptr<StudyFont> cjkFont;
  std::unique_ptr<StudyFont> thaiFont;
  bool cjkTried = false;
  bool thaiTried = false;
};
