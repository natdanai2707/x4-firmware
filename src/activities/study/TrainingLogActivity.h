#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "study/Training.h"

// Logs one planned session: status, distance, pace, average HR, RPE and a
// note. Up/Down pick a field, Left/Right change it; touch uses the - and +
// buttons (the outer ones take big steps). Save queues the change for Notion
// and finishes with a non-cancelled result; Back leaves without saving.
class TrainingLogActivity final : public Activity {
 public:
  TrainingLogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, training::Session session);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // The session with the logged values, read by the caller after a save.
  const training::Session& result() const { return session; }

 private:
  enum Field : int { Status, Km, Pace, Hr, Rpe, Notes, Save, FIELD_COUNT };
  void change(int field, int steps);
  void editNotes();
  void save();
  std::string valueText(int field) const;
  bool fieldShown(int field) const;

  training::Session session;
  int status = 0;  // index in STATUSES
  float km = 0;
  int paceSec = 0;
  int hr = 0;
  int rpe = 0;
  std::string notes;
  int focus = Status;
  bool swallowBack = false;

  struct Hit {
    int x0, y0, x1, y1;
    int field;
    int steps;  // 0: select or act
  };
  std::vector<Hit> hits;
};
