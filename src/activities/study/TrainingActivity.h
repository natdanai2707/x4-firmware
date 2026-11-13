#pragma once

#include <ctime>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "study/Training.h"

// Training on the device, so a workout does not need the phone. It opens on a
// picker (Run, Gym, Grip, Hyrox) that does not touch WiFi; only Run syncs on
// open. Same plan and
// same Notion "Sessions" database as the Training Hub phone app:
//   Today  the week around today from Notion; a run is logged in
//          TrainingLogActivity (status, km, pace, HR, RPE, notes).
//   Gym    Push / Legs / Pull: tick sets, set loads, save to Notion.
//   Grip   the grip work of each weekday, same as Gym.
//   Hyrox  Mini Hyrox stations (30 or 45 min), same as Gym, no timer.
//          Hyrox logs are kept off the Today week.
// Logs are queued on the card and sent when WiFi is available, then the plan
// is pulled again so phone and device show the same thing.
//
// Setup: put the Notion integration key in notion.txt on the card root; it
// moves into /.crosspoint/training/ on first use.
class TrainingActivity final : public Activity {
 public:
  TrainingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return syncing; }

 private:
  enum class Tab : uint8_t { Today, Gym, Grip, Hyrox };
  static constexpr int TAB_COUNT = 4;

  // Ticked sets of the open Gym or Grip day.
  struct Lift {
    const training::LiftDay* day = nullptr;
    std::vector<int> done;  // sets ticked per exercise
    std::vector<float> loads;
    int focus = 0;  // exercise index, count = Save
    int first = 0;  // first visible row
  };

  void setTab(Tab t);
  void startSync();
  void runSync();
  void stopWifi();
  bool wifiSaved() const;
  void openLog(size_t sessionIndex);
  void setDay(const std::string& iso);
  std::vector<size_t> sessionsOn(const std::string& iso) const;

  Lift& lift() { return tab == Tab::Gym ? gym : tab == Tab::Grip ? grip : hyrox; }
  // Day chips of a lift tab: chip `i`, and the chip count in `count`.
  static const training::LiftDay* chipDay(Tab t, int i, int& count);
  const char* liftName(const Lift& l) const;
  void openLiftDay(Lift& l, const training::LiftDay* day);
  void tick(Lift& l, int exercise, int box);
  void changeLoad(Lift& l, int exercise, int dir);
  void saveLift(Lift& l);
  void saveTicks() const;
  void loadTicks();

  int contentTop() const;
  int tabBarTop() const;
  void renderStatusLine(int y, const char* title);
  void renderToday(int y);
  void renderLift(int y);
  void renderTabBar();
  void renderPicker();
  void pick(int index);

  std::vector<training::Session> sessions;
  std::string todayIso;
  std::string dayIso;  // selected day on Today
  std::string syncStatus;
  bool syncing = false;
  bool syncPending = false;
  bool syncDue = false;  // sync when Run opens; the picker never connects
  bool picking = true;   // the Run / Gym / Grip / Hyrox picker is showing
  int pickSel = 0;
  bool swallowBack = false;
  Tab tab = Tab::Today;
  Lift gym;
  Lift grip;
  Lift hyrox;
  time_t restUntil = 0;
  int restSeconds = 0;

  // Hit areas from the last render.
  struct Hit {
    int x0, y0, x1, y1;
    int kind;  // see TrainingActivity.cpp
    int a, b;
  };
  std::vector<Hit> hits;
  int statusTop = 0;
  int statusBottom = 0;
};
