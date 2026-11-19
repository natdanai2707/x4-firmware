#pragma once

#include <string>
#include <vector>

// Training plan, local store and Notion sync for the Training app. The Notion
// "Sessions" database is the source of truth, shared with the phone app: the
// device keeps a copy of the sessions around today, and every log is queued
// in an outbox first so it survives a missing network and is sent on the
// next sync.
namespace training {

struct Session {
  std::string id;    // Notion page id; empty for a log not yet synced
  std::string date;  // YYYY-MM-DD
  std::string title;
  std::string type;
  std::string status;
  std::string workout;
  std::string targetPace;
  std::string actualPace;
  std::string notes;
  float plannedKm = 0;
  float actualKm = 0;
  int rpe = 0;
  int hr = 0;
  int week = 0;
  bool done() const { return status == "Done" || status == "Modified"; }
  bool isRun() const;
};

struct Exercise {
  const char* key;
  const char* name;
  const char* target;
  int sets;
  const char* unit;  // "kg", "s", "cm", or "" for bodyweight
  float def;
  int rest;                         // seconds
  const char* cue = nullptr;        // coaching cue, or the Hyrox substitute note
  const char* shortName = nullptr;  // step strip label
  const char* rir = nullptr;        // reps in reserve, e.g. "RIR 1-2"
  int superset = 0;                 // 1-based partner exercise, 0 for none
};

struct LiftDay {
  const char* key;
  const char* label;
  const char* type;  // Notion Type
  int dow;           // 0 Sun ... 6 Sat, -1 none
  const char* note;  // shown above the exercises, may be nullptr
  const Exercise* ex;
  int count;
};

// Upper A, Arms, Upper B, Legs; LiftDay::dow is the planned weekday.
const LiftDay* gymDays(int& count);
// Mini Hyrox programs (45 and 60 min): one station per round, a run before
// each. Not tied to a weekday.
const LiftDay* hyroxDays(int& count);
// Grip work for a weekday (1 Mon ... 5 Fri), nullptr otherwise.
const LiftDay* gripDay(int dow);

// The plan's gym day for a Notion type (Upper A, Upper B, Arms, Strength, and
// the older Push, Pull, Upper), or nullptr when it is not a gym type.
const LiftDay* gymDayForType(const std::string& type);
// The gym day on `iso`, as the phone app schedules it: a gym row in Notion on
// that date wins; otherwise the plan's weekday, unless that program already
// has a row on another day of the same week (planned, done or skipped), or
// the date is outside the block.
const LiftDay* gymDayOn(const std::string& iso, const std::vector<Session>& sessions);

std::string today();  // "" when the clock is not set
std::string addDays(const std::string& iso, int days);
int weekday(const std::string& iso);  // 0 Sun ... 6 Sat, -1 if invalid
int weekOf(const std::string& iso);   // block week, 0 before the block

// Cached sessions.
void loadSessions(std::vector<Session>& out);
bool saveSessions(const std::vector<Session>& sessions);

// Remembered loads per exercise key.
float loadFor(const Exercise& e);
void setLoad(const Exercise& e, float value);

// Queued Notion writes. pageId empty creates a page.
struct Change {
  std::string pageId;
  std::string title;
  std::string date;
  std::string type;
  std::string status;
  std::string workout;
  std::string notes;
  std::string targetPace;
  std::string actualPace;
  float actualKm = -1;  // < 0: not sent
  int rpe = 0;          // 0: not sent
  int hr = 0;           // 0: not sent
  int week = -1;        // < 0: not sent
};
bool queue(const Change& change);
int pendingCount();

bool hasToken();
// Sends queued writes, then refreshes the cached sessions around today.
// Needs WiFi. `message` is a short status line for the screen.
bool sync(std::vector<Session>& sessions, std::string& message);

}  // namespace training
