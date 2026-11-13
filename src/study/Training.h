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
  const char* unit;  // "kg" or "s"
  float def;
  int rest;  // seconds
};

struct LiftDay {
  const char* key;
  const char* label;
  const char* type;  // Notion Type
  int dow;           // 0 Sun ... 6 Sat
  const Exercise* ex;
  int count;
};

// Push, Legs, Pull; LiftDay::dow is the planned weekday.
const LiftDay* gymDays(int& count);
// Grip work for a weekday (1 Mon ... 5 Fri), nullptr otherwise.
const LiftDay* gripDay(int dow);

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
