#include "Training.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "network/HttpDownloader.h"
#include "util/PluginHttp.h"

namespace training {
namespace {

constexpr char DIR[] = "/.crosspoint/training";
constexpr char SESSIONS_PATH[] = "/.crosspoint/training/sessions.tsv";
constexpr char OUTBOX_PATH[] = "/.crosspoint/training/outbox.tsv";
constexpr char LOADS_PATH[] = "/.crosspoint/training/loads.txt";
constexpr char TOKEN_PATH[] = "/.crosspoint/training/notion.txt";
// Dropped on the card root once; moved into the hidden folder on first use.
constexpr char TOKEN_DROP_PATH[] = "/notion.txt";
constexpr char QUERY_PATH[] = "/.crosspoint/training/query.json";
constexpr char REJECTED_PATH[] = "/.crosspoint/training/rejected.tsv";
constexpr size_t MAX_FILE = 64 * 1024;
constexpr size_t MAX_QUERY = 192 * 1024;
constexpr int MAX_PAGES = 4;

// Notion Sessions database of "10k Plan: Block 1".
constexpr char DATA_SOURCE[] = "1a66fbd0-3df7-41d9-9b26-408d96865265";
constexpr char API[] = "https://api.notion.com/v1/";
constexpr char NOTION_VERSION[] = "2025-09-03";
constexpr char BLOCK_START[] = "2026-11-02";  // Week 1, Monday
// Sessions kept on the device: last week through next week.
constexpr int WINDOW_BACK = 7;
constexpr int WINDOW_AHEAD = 14;

constexpr Exercise PUSH[] = {
    {"bench", "Bench press", "4 x 6-8", 4, "kg", 50, 120},
    {"incdb", "Incline dumbbell press", "3 x 8-10", 3, "kg", 18, 90},
    {"ohp", "Seated dumbbell shoulder press", "3 x 8-10", 3, "kg", 14, 90},
    {"lat", "Lateral raise", "4 x 12-15", 4, "kg", 7, 60},
    {"push", "Triceps rope pushdown", "3 x 10-12", 3, "kg", 20, 60},
    {"ohtri", "Overhead triceps extension", "3 x 10-12", 3, "kg", 15, 60},
};
constexpr Exercise LEGS[] = {
    {"squat", "Squat", "3 x 6-8", 3, "kg", 24, 120},
    {"rdl", "Romanian deadlift", "3 x 8", 3, "kg", 50, 120},
    {"bss", "Bulgarian split squat", "3 x 8 per leg", 3, "kg", 12, 90},
    {"stepup", "Knee-height step-up", "3 x 8 per leg", 3, "kg", 10, 60},
    {"calf", "Bent-knee calf raise", "3 x 12", 3, "kg", 20, 60},
    {"plank", "Side plank", "2 x 30s per side", 2, "s", 30, 45},
};
constexpr Exercise PULL[] = {
    {"pullup", "Pull-up or lat pulldown", "4 x 6-10", 4, "kg", 0, 120},
    {"csrow", "Chest-supported row", "4 x 8-10", 4, "kg", 40, 90},
    {"dbrow", "One-arm dumbbell row", "3 x 10 per side", 3, "kg", 24, 60},
    {"face", "Face pull", "3 x 15", 3, "kg", 15, 60},
    {"inccurl", "Incline dumbbell curl", "3 x 8-12", 3, "kg", 9, 60},
    {"hamcurl", "Hammer curl (heavy)", "3 x 8-10", 3, "kg", 12, 60},
};
constexpr LiftDay GYM[] = {
    {"push", "Push", "Push", 1, nullptr, PUSH, 6},
    {"legs", "Legs", "Strength", 3, nullptr, LEGS, 6},
    {"pull", "Pull", "Pull", 5, nullptr, PULL, 6},
};

constexpr Exercise GRIP_MON[] = {{"wrist", "Wrist curl", "5 x 15-25", 5, "kg", 5, 60}};
constexpr Exercise GRIP_TUE[] = {{"hammer", "Hammer curl", "4 x 12-15", 4, "kg", 8, 60}};
constexpr Exercise GRIP_WED[] = {{"hang", "Dead hang / block lift", "5 x 20-30s", 5, "s", 25, 90}};
constexpr Exercise GRIP_THU[] = {{"rwrist", "Reverse wrist curl", "3 x 20", 3, "kg", 3, 45},
                                 {"hammer", "Hammer curl", "3 x 12", 3, "kg", 8, 60}};
constexpr Exercise GRIP_FRI[] = {{"finger", "Finger curl", "4 x 20", 4, "kg", 5, 60}};
constexpr LiftDay GRIP[] = {
    {"mon", "Mon", "Grip", 1, nullptr, GRIP_MON, 1}, {"tue", "Tue", "Grip", 2, nullptr, GRIP_TUE, 1},
    {"wed", "Wed", "Grip", 3, nullptr, GRIP_WED, 1}, {"thu", "Thu", "Grip", 4, nullptr, GRIP_THU, 2},
    {"fri", "Fri", "Grip", 5, nullptr, GRIP_FRI, 1},
};

// Station loads are remembered apart from the gym ones (hx_ keys).
constexpr Exercise HYROX_30[] = {
    {"hx_swing", "Kettlebell swing", "40 reps", 1, "kg", 16, 0},
    {"hx_row", "Dumbbell bent-over row", "30 reps", 1, "kg", 12, 0},
    {"hx_burpee", "Burpee", "20 reps", 1, "", 0, 0},
    {"hx_farmer", "Farmer carry", "60 sec", 1, "kg", 20, 0},
    {"hx_lunge", "Sandbag lunge", "30 reps", 1, "kg", 20, 0},
    {"hx_thruster", "Dumbbell thruster", "30 reps", 1, "kg", 10, 0},
};
constexpr Exercise HYROX_45[] = {
    {"hx_swing", "Kettlebell swing", "50 reps", 1, "kg", 16, 0},
    {"hx_burpee", "Burpee", "25 reps", 1, "", 0, 0},
    {"hx_lunge", "Sandbag lunge", "40 reps", 1, "kg", 20, 0},
    {"hx_row", "Dumbbell bent-over row", "40 reps", 1, "kg", 12, 0},
    {"hx_farmer", "Farmer carry", "90 sec", 1, "kg", 20, 0},
    {"hx_press", "Dumbbell press", "40 reps", 1, "kg", 12, 0},
};
constexpr LiftDay HYROX[] = {
    {"p30", "30 min", "Hyrox", -1, "6 rounds. Run 2 min, then the station.", HYROX_30, 6},
    {"p45", "45 min", "Hyrox", -1, "6 rounds. Run 4 min, then the station.", HYROX_45, 6},
};

constexpr const char* RUN_TYPES[] = {"Easy", "Threshold", "10k Specific", "Long Run", "Time Trial", "Race"};

bool parseIso(const std::string& iso, struct tm& t) {
  if (iso.size() < 10) return false;
  t = {};
  t.tm_year = std::atoi(iso.substr(0, 4).c_str()) - 1900;
  t.tm_mon = std::atoi(iso.substr(5, 2).c_str()) - 1;
  t.tm_mday = std::atoi(iso.substr(8, 2).c_str());
  t.tm_hour = 12;  // away from DST edges
  return t.tm_year > 100 && t.tm_mon >= 0 && t.tm_mon < 12 && t.tm_mday > 0;
}

std::string formatIso(const struct tm& t) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return buf;
}

std::string clean(std::string v) {
  std::replace(v.begin(), v.end(), '\t', ' ');
  std::replace(v.begin(), v.end(), '\n', ' ');
  std::replace(v.begin(), v.end(), '\r', ' ');
  return v;
}

std::string trim(std::string s) {
  const auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t tab = line.find('\t', start);
    out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
    if (tab == std::string::npos) break;
    start = tab + 1;
  }
  return out;
}

template <typename F>
void forEachLine(const std::string& text, F f) {
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line[0] != '#') f(line);
  }
}

std::string num(float v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

std::string readToken() {
  Storage.ensureDirectoryExists(DIR);
  if (Storage.exists(TOKEN_DROP_PATH)) {
    std::string dropped;
    if (Storage.readFileToString("TRAIN", TOKEN_DROP_PATH, 512, dropped) && !trim(dropped).empty()) {
      Storage.writeFile(TOKEN_PATH, String(trim(dropped).c_str()));
    }
    Storage.remove(TOKEN_DROP_PATH);
  }
  std::string text;
  if (!Storage.exists(TOKEN_PATH) || !Storage.readFileToString("TRAIN", TOKEN_PATH, 512, text)) return "";
  text = trim(text);
  const size_t eq = text.find('=');
  if (eq != std::string::npos && text.compare(0, eq, "token") == 0) text = trim(text.substr(eq + 1));
  return text;
}

void loadChanges(std::vector<Change>& out) {
  out.clear();
  std::string text;
  if (!Storage.exists(OUTBOX_PATH) || !Storage.readFileToString("TRAIN", OUTBOX_PATH, MAX_FILE, text)) return;
  forEachLine(text, [&](const std::string& line) {
    const auto f = splitTabs(line);
    if (f.size() < 13) return;
    Change c;
    c.pageId = f[0];
    c.title = f[1];
    c.date = f[2];
    c.type = f[3];
    c.status = f[4];
    c.workout = f[5];
    c.notes = f[6];
    c.targetPace = f[7];
    c.actualPace = f[8];
    c.actualKm = std::strtof(f[9].c_str(), nullptr);
    c.rpe = std::atoi(f[10].c_str());
    c.hr = std::atoi(f[11].c_str());
    c.week = std::atoi(f[12].c_str());
    out.push_back(std::move(c));
  });
}

std::string changeLine(const Change& c) {
  return clean(c.pageId) + "\t" + clean(c.title) + "\t" + clean(c.date) + "\t" + clean(c.type) + "\t" +
         clean(c.status) + "\t" + clean(c.workout) + "\t" + clean(c.notes) + "\t" + clean(c.targetPace) + "\t" +
         clean(c.actualPace) + "\t" + num(c.actualKm) + "\t" + std::to_string(c.rpe) + "\t" + std::to_string(c.hr) +
         "\t" + std::to_string(c.week) + "\n";
}

bool saveChanges(const std::vector<Change>& changes, const char* path) {
  if (changes.empty()) {
    if (Storage.exists(path)) Storage.remove(path);
    return true;
  }
  std::string text;
  for (const auto& c : changes) text += changeLine(c);
  return Storage.writeFile(path, String(text.c_str()));
}

void richText(JsonObject props, const char* name, const std::string& value) {
  // Notion caps one text object at 2000 characters.
  props[name]["rich_text"][0]["text"]["content"] = value.substr(0, 1900);
}

// Notion request body for one queued change.
std::string changeBody(const Change& c) {
  JsonDocument doc;
  if (c.pageId.empty()) {
    doc["parent"]["type"] = "data_source_id";
    doc["parent"]["data_source_id"] = DATA_SOURCE;
  }
  JsonObject p = doc["properties"].to<JsonObject>();
  if (!c.title.empty()) p["Session"]["title"][0]["text"]["content"] = c.title;
  if (!c.date.empty()) p["Date"]["date"]["start"] = c.date;
  if (!c.type.empty()) p["Type"]["select"]["name"] = c.type;
  if (!c.status.empty()) p["Status"]["select"]["name"] = c.status;
  if (!c.workout.empty()) richText(p, "Workout", c.workout);
  if (!c.notes.empty()) richText(p, "Notes", c.notes);
  if (!c.targetPace.empty()) richText(p, "Target Pace", c.targetPace);
  if (!c.actualPace.empty()) richText(p, "Actual Pace", c.actualPace);
  if (c.actualKm >= 0) p["Actual km"]["number"] = c.actualKm;
  if (c.rpe > 0) p["RPE"]["number"] = c.rpe;
  if (c.hr > 0) p["Avg HR"]["number"] = c.hr;
  if (c.week >= 0) p["Week"]["number"] = c.week;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string plainText(JsonVariantConst arr) {
  std::string out;
  for (JsonVariantConst part : arr.as<JsonArrayConst>()) out += part["plain_text"] | "";
  return out;
}

// Reads one query response page into `out`; returns next_cursor or "".
std::string parseQuery(std::vector<Session>& out, bool& ok) {
  ok = false;
  HalFile file;
  if (!Storage.openFileForRead("TRAIN", QUERY_PATH, file)) return "";
  JsonDocument filter;
  filter["has_more"] = true;
  filter["next_cursor"] = true;
  JsonObject r = filter["results"][0].to<JsonObject>();
  r["id"] = true;
  JsonObject p = r["properties"].to<JsonObject>();
  p["Session"]["title"][0]["plain_text"] = true;
  for (const char* name : {"Workout", "Target Pace", "Actual Pace", "Notes"}) {
    p[name]["rich_text"][0]["plain_text"] = true;
  }
  for (const char* name : {"Planned km", "Actual km", "RPE", "Avg HR", "Week"}) p[name]["number"] = true;
  p["Type"]["select"]["name"] = true;
  p["Status"]["select"]["name"] = true;
  p["Date"]["date"]["start"] = true;

  JsonDocument doc;
  const auto err = deserializeJson(doc, file, DeserializationOption::Filter(filter));
  file.close();
  Storage.remove(QUERY_PATH);
  if (err) {
    LOG_ERR("TRAIN", "Query parse error: %s", err.c_str());
    return "";
  }
  for (JsonVariantConst row : doc["results"].as<JsonArrayConst>()) {
    JsonVariantConst props = row["properties"];
    Session s;
    s.id = row["id"] | "";
    s.title = plainText(props["Session"]["title"]);
    s.date = std::string(props["Date"]["date"]["start"] | "").substr(0, 10);
    s.type = props["Type"]["select"]["name"] | "";
    s.status = props["Status"]["select"]["name"] | "";
    s.workout = plainText(props["Workout"]["rich_text"]);
    s.targetPace = plainText(props["Target Pace"]["rich_text"]);
    s.actualPace = plainText(props["Actual Pace"]["rich_text"]);
    s.notes = plainText(props["Notes"]["rich_text"]);
    s.plannedKm = props["Planned km"]["number"] | 0.0f;
    s.actualKm = props["Actual km"]["number"] | 0.0f;
    s.rpe = props["RPE"]["number"] | 0;
    s.hr = props["Avg HR"]["number"] | 0;
    s.week = props["Week"]["number"] | 0;
    if (!s.date.empty()) out.push_back(std::move(s));
  }
  ok = true;
  const bool more = doc["has_more"] | false;
  return more ? std::string(doc["next_cursor"] | "") : std::string();
}

}  // namespace

bool Session::isRun() const {
  return std::any_of(std::begin(RUN_TYPES), std::end(RUN_TYPES), [&](const char* t) { return type == t; });
}

const LiftDay* gymDays(int& count) {
  count = static_cast<int>(sizeof(GYM) / sizeof(GYM[0]));
  return GYM;
}

const LiftDay* hyroxDays(int& count) {
  count = static_cast<int>(sizeof(HYROX) / sizeof(HYROX[0]));
  return HYROX;
}

const LiftDay* gripDay(const int dow) {
  for (const auto& d : GRIP) {
    if (d.dow == dow) return &d;
  }
  return nullptr;
}

std::string today() {
  const time_t now = time(nullptr);
  if (now < 1577836800) return "";
  struct tm t{};
  localtime_r(&now, &t);
  return formatIso(t);
}

std::string addDays(const std::string& iso, const int days) {
  struct tm t{};
  if (!parseIso(iso, t)) return iso;
  t.tm_mday += days;
  mktime(&t);
  return formatIso(t);
}

int weekday(const std::string& iso) {
  struct tm t{};
  if (!parseIso(iso, t)) return -1;
  mktime(&t);
  return t.tm_wday;
}

int weekOf(const std::string& iso) {
  struct tm a{};
  struct tm b{};
  if (!parseIso(iso, a) || !parseIso(BLOCK_START, b)) return 0;
  const double days = difftime(mktime(&a), mktime(&b)) / 86400.0;
  if (days < 0) return 0;
  return static_cast<int>(days + 0.5) / 7 + 1;
}

void loadSessions(std::vector<Session>& out) {
  out.clear();
  std::string text;
  if (!Storage.exists(SESSIONS_PATH) || !Storage.readFileToString("TRAIN", SESSIONS_PATH, MAX_FILE, text)) return;
  forEachLine(text, [&](const std::string& line) {
    const auto f = splitTabs(line);
    if (f.size() < 15) return;
    Session s;
    s.id = f[0];
    s.date = f[1];
    s.title = f[2];
    s.type = f[3];
    s.status = f[4];
    s.workout = f[5];
    s.targetPace = f[6];
    s.actualPace = f[7];
    s.notes = f[8];
    s.plannedKm = std::strtof(f[9].c_str(), nullptr);
    s.actualKm = std::strtof(f[10].c_str(), nullptr);
    s.rpe = std::atoi(f[11].c_str());
    s.hr = std::atoi(f[12].c_str());
    s.week = std::atoi(f[13].c_str());
    out.push_back(std::move(s));
  });
}

bool saveSessions(const std::vector<Session>& sessions) {
  Storage.ensureDirectoryExists(DIR);
  std::string text = "# id\tdate\ttitle\ttype\tstatus\tworkout\ttarget\tpace\tnotes\tplanned\tkm\trpe\thr\tweek\t-\n";
  for (const auto& s : sessions) {
    text += clean(s.id) + "\t" + clean(s.date) + "\t" + clean(s.title) + "\t" + clean(s.type) + "\t" + clean(s.status) +
            "\t" + clean(s.workout) + "\t" + clean(s.targetPace) + "\t" + clean(s.actualPace) + "\t" + clean(s.notes) +
            "\t" + num(s.plannedKm) + "\t" + num(s.actualKm) + "\t" + std::to_string(s.rpe) + "\t" +
            std::to_string(s.hr) + "\t" + std::to_string(s.week) + "\t-\n";
  }
  return Storage.writeFile(SESSIONS_PATH, String(text.c_str()));
}

float loadFor(const Exercise& e) {
  std::string text;
  if (Storage.exists(LOADS_PATH)) Storage.readFileToString("TRAIN", LOADS_PATH, 4096, text);
  float value = e.def;
  forEachLine(text, [&](const std::string& line) {
    const size_t eq = line.find('=');
    if (eq != std::string::npos && line.compare(0, eq, e.key) == 0) value = std::strtof(line.c_str() + eq + 1, nullptr);
  });
  return value;
}

void setLoad(const Exercise& e, const float value) {
  std::string text;
  if (Storage.exists(LOADS_PATH)) Storage.readFileToString("TRAIN", LOADS_PATH, 4096, text);
  std::string out;
  forEachLine(text, [&](const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.compare(0, eq, e.key) != 0) out += line + "\n";
  });
  out += std::string(e.key) + "=" + num(value) + "\n";
  Storage.ensureDirectoryExists(DIR);
  Storage.writeFile(LOADS_PATH, String(out.c_str()));
}

bool queue(const Change& change) {
  Storage.ensureDirectoryExists(DIR);
  std::vector<Change> changes;
  loadChanges(changes);
  changes.push_back(change);
  return saveChanges(changes, OUTBOX_PATH);
}

int pendingCount() {
  std::vector<Change> changes;
  loadChanges(changes);
  return static_cast<int>(changes.size());
}

bool hasToken() { return !readToken().empty(); }

bool sync(std::vector<Session>& sessions, std::string& message) {
  const std::string token = readToken();
  if (token.empty()) {
    message = "No Notion key: put notion.txt on the card";
    return false;
  }
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    message = "Sync failed: low memory";
    return false;
  }
  const pluginhttp::Headers headers = {
      {"Authorization", "Bearer " + token}, {"Notion-Version", NOTION_VERSION}, {"Content-Type", "application/json"}};
  // One TLS session for every request: repeated handshakes fragment the heap.
  freeink::SecureHttpClient session;

  // 1. Send queued logs, oldest first. A change Notion rejects as invalid is
  // set aside so it cannot block the rest.
  std::vector<Change> changes;
  loadChanges(changes);
  std::vector<Change> rejected;
  size_t sent = 0;
  while (sent < changes.size()) {
    const Change& c = changes[sent];
    const std::string url = c.pageId.empty() ? std::string(API) + "pages" : std::string(API) + "pages/" + c.pageId;
    String response;
    const int status = pluginhttp::request(&session, url, c.pageId.empty() ? "POST" : "PATCH", changeBody(c), headers,
                                           response, 8 * 1024);
    if (status >= 200 && status < 300) {
      ++sent;
      continue;
    }
    LOG_ERR("TRAIN", "Notion write HTTP %d: %s", status, response.substring(0, 200).c_str());
    if (status == 400 || status == 404) {
      rejected.push_back(c);
      ++sent;
      continue;
    }
    break;
  }
  changes.erase(changes.begin(), changes.begin() + static_cast<long>(sent));
  saveChanges(changes, OUTBOX_PATH);
  if (!rejected.empty()) {
    std::string text;
    if (Storage.exists(REJECTED_PATH)) Storage.readFileToString("TRAIN", REJECTED_PATH, MAX_FILE, text);
    for (const auto& c : rejected) text += changeLine(c);
    Storage.writeFile(REJECTED_PATH, String(text.c_str()));
  }
  if (!changes.empty()) {
    message = "Saved here, " + std::to_string(changes.size()) + " log(s) waiting for Notion";
    return false;
  }

  // 2. Refresh the sessions around today.
  const std::string now = today();
  if (now.empty()) {
    message = "Sent. Set the clock to load the plan";
    return true;
  }
  const std::string url = std::string(API) + "data_sources/" + DATA_SOURCE + "/query";
  std::vector<Session> fresh;
  std::string cursor;
  for (int page = 0; page < MAX_PAGES; ++page) {
    JsonDocument body;
    JsonArray and_ = body["filter"]["and"].to<JsonArray>();
    JsonObject from = and_.add<JsonObject>();
    from["property"] = "Date";
    from["date"]["on_or_after"] = addDays(now, -WINDOW_BACK);
    JsonObject to = and_.add<JsonObject>();
    to["property"] = "Date";
    to["date"]["on_or_before"] = addDays(now, WINDOW_AHEAD);
    body["sorts"][0]["property"] = "Date";
    body["sorts"][0]["direction"] = "ascending";
    body["page_size"] = 50;
    if (!cursor.empty()) body["start_cursor"] = cursor;
    std::string json;
    serializeJson(body, json);
    const int status = pluginhttp::requestToFile(&session, url, "POST", json, headers, QUERY_PATH, MAX_QUERY);
    if (status < 200 || status >= 300) {
      Storage.remove(QUERY_PATH);
      LOG_ERR("TRAIN", "Notion query HTTP %d", status);
      message = status == 401 || status == 403 || status == 404
                    ? "Notion refused (HTTP " + std::to_string(status) + "): check key and sharing"
                    : "Sync failed: no connection to Notion";
      return false;
    }
    bool ok = false;
    cursor = parseQuery(fresh, ok);
    if (!ok) {
      message = "Sync failed: unreadable answer";
      return false;
    }
    if (cursor.empty()) break;
  }
  sessions = std::move(fresh);
  saveSessions(sessions);
  message = rejected.empty() ? "Synced with Notion" : "Synced, " + std::to_string(rejected.size()) + " log(s) rejected";
  return true;
}

}  // namespace training
