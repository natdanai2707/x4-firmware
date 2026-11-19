#pragma once

#include <ctime>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "study/Training.h"

// Training on the device, so a workout does not need the phone. Same plan and
// same Notion "Sessions" database as the Training Hub phone app. Five tabs:
//   Calendar  the week: what each day holds (runs from Notion, the gym and
//             grip plan, Hyrox), ticked when logged. Tap a day for its cards.
//   Run       the week's runs, logged in TrainingLogActivity, and mileage.
//   Gym       Upper A, Arms, Upper B, Legs; a session goes one exercise at a
//             time: tick sets, set the load, rest, then save to Notion.
//   Grip      the grip work of each weekday.
//   Hyrox     Mini Hyrox 45 or 60 min: tick each station, no timer.
// Opening the app never turns WiFi on: Run syncs on open when the plan is old
// or logs are waiting, and the header's sync status syncs on tap. Logs are
// queued on the card and sent with the next sync.
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
  enum class Tab : uint8_t { Calendar, Run, Gym, Grip, Hyrox };
  static constexpr int TAB_COUNT = 5;

  // Ticked sets of the open Gym, Grip or Hyrox day.
  struct Lift {
    const training::LiftDay* day = nullptr;
    std::vector<int> done;  // sets ticked per exercise
    std::vector<float> loads;
    int focus = 0;  // exercise index; count = Finish
  };

  // One line of a Calendar day.
  enum class Kind : uint8_t { Run, Hyrox, Gym, Grip, Other };
  struct Item {
    Kind kind;
    std::string text;
    bool done = false;
    int session = -1;  // index into sessions, -1 for a plan-only item
    const training::LiftDay* day = nullptr;
  };

  void setTab(Tab t);
  void startSync();
  void runSync();
  void stopWifi();
  bool wifiSaved() const;
  void openLog(size_t sessionIndex);
  std::vector<size_t> sessionsOn(const std::string& iso) const;
  std::vector<Item> itemsOn(const std::string& iso) const;
  bool loggedThisWeek(const char* type, const std::string& iso) const;

  Lift& lift() { return tab == Tab::Gym ? gym : tab == Tab::Grip ? grip : hyrox; }
  static const training::LiftDay* chipDay(Tab t, int i, int& count);
  const char* liftName(const Lift& l) const;
  void openLiftDay(Lift& l, const training::LiftDay* day);
  void startLift(Tab t, const training::LiftDay* day);
  void tick(Lift& l, int exercise, int box);
  void changeLoad(Lift& l, int exercise, int dir);
  void clearLift(Lift& l);
  void primary();  // the big button of the open Gym step
  void saveLift(Lift& l);
  void saveTicks() const;
  void loadTicks();

  int contentTop() const;
  int tabBarTop() const;
  void renderCalendar(int y);
  void renderDay(int y);
  void renderRun(int y);
  void renderGymList(int y);
  void renderGymStep(int y);
  void renderGrip(int y);
  void renderHyrox(int y);
  void renderTabBar();
  int renderWeekNav(int y, const std::string& monday, const std::string& sub);
  int renderSetRow(int x, int y, int width, const Lift& l, int exercise, int height);
  int renderLoadRow(int x, int y, int width, const Lift& l, int exercise, const char* label);
  std::string syncText() const;

  std::vector<training::Session> sessions;
  std::string todayIso;
  std::string weekStart;     // Monday of the Calendar week
  std::string runWeekStart;  // Monday of the Run week
  std::string dayIso;        // Calendar day open in the day view
  bool dayOpen = false;
  bool gymOpen = false;  // a Gym session is open (else the day list)
  int calSel = 0;        // Calendar row picked with the buttons, Mon = 0
  bool hxEdit = false;   // the focused Hyrox row shows - and +
  std::string syncStatus;
  std::string lastSyncText;
  bool syncing = false;
  bool syncPending = false;
  bool syncDue = false;
  bool swallowBack = false;
  Tab tab = Tab::Calendar;
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
};
