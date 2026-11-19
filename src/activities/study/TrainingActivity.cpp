#include "TrainingActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/study/TrainingLogActivity.h"
#include "components/UITheme.h"
#include "components/icons/trainIcons.h"
#include "fontIds.h"

namespace {
constexpr char TICKS_PATH[] = "/.crosspoint/training/ticks.txt";
constexpr char LAST_SYNC_PATH[] = "/.crosspoint/training/last_sync.txt";
// Pull the plan again when Run opens and the last sync is older than this.
constexpr time_t RESYNC_SECONDS = 60 * 60;
constexpr int GAP = 12;
constexpr int TAB_BAR_HEIGHT = 64;
constexpr int NAV_H = 56;
constexpr int BTN_H = 52;
constexpr int RADIUS = 12;
constexpr int ICON = 24;
constexpr const char* TAB_NAMES[] = {"Calendar", "Run", "Gym", "Grip", "Hyrox"};
constexpr const uint8_t* TAB_ICONS[] = {TrainCalIcon, TrainRunIcon, TrainGymIcon, TrainGripIcon, TrainHyroxIcon};
constexpr const char* DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* DAYS_LONG[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
constexpr const char* GYM_TYPES[] = {"Upper A", "Upper B", "Arms", "Strength", "Push", "Pull", "Upper"};

enum HitKind : int {
  DayRow = 1,
  PrevWeek,
  NextWeek,
  CloseDay,
  OpenLog,
  StartGym,
  StartGrip,
  GoTab,
  Step,
  SetBox,
  LoadDown,
  LoadUp,
  Primary,
  SaveLift,
  ClearLift,
  Chip,
  HxToggle,
  HxFocus,
  CloseGym,
  Sync,
};

int dayOfMonth(const std::string& iso) { return iso.size() >= 10 ? std::atoi(iso.substr(8, 2).c_str()) : 0; }

const char* monthName(const std::string& iso) {
  const int m = iso.size() >= 7 ? std::atoi(iso.substr(5, 2).c_str()) : 0;
  return m >= 1 && m <= 12 ? MONTHS[m - 1] : "";
}

std::string mondayOf(const std::string& iso) {
  const int wd = training::weekday(iso);
  return wd < 0 ? iso : training::addDays(iso, -((wd + 6) % 7));
}

// "16 - 22 Nov", or "30 Nov - 6 Dec" across months.
std::string weekRange(const std::string& monday) {
  const std::string sunday = training::addDays(monday, 6);
  std::string a = std::to_string(dayOfMonth(monday));
  if (std::string(monthName(monday)) != monthName(sunday)) a += std::string(" ") + monthName(monday);
  return a + " - " + std::to_string(dayOfMonth(sunday)) + " " + monthName(sunday);
}

// "2026-11-13" -> "11/13", as the phone app names its logs.
std::string shortDay(const std::string& iso) {
  return iso.size() >= 10 ? iso.substr(5, 2) + "/" + iso.substr(8, 2) : iso;
}

// "W3 Thu: Easy 7 km" -> "Easy 7 km".
std::string planTitle(const std::string& title) {
  if (title.size() > 2 && title[0] == 'W' && isdigit(static_cast<unsigned char>(title[1]))) {
    const size_t colon = title.find(": ");
    if (colon != std::string::npos && colon < 12) return title.substr(colon + 2);
  }
  return title;
}

std::string num(const float v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

std::string loadText(const float v, const char* unit) {
  char buf[24];
  if (v == static_cast<int>(v)) {
    snprintf(buf, sizeof(buf), "%d %s", static_cast<int>(v), unit);
  } else {
    snprintf(buf, sizeof(buf), "%.1f %s", v, unit);
  }
  return buf;
}

std::string clockText(const time_t t) {
  struct tm tm{};
  localtime_r(&t, &tm);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

std::string mmss(const int s) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
  return buf;
}

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::toupper);
  return s;
}

void drawCentered(const GfxRenderer& r, int font, int x, int y, int w, int h, const char* text, bool black,
                  EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int tw = r.getTextWidth(font, text, style);
  r.drawText(font, x + (w - tw) / 2, y + (h - r.getLineHeight(font)) / 2, text, black, style);
}

// A small label box; filled = black with white text. Returns its width.
int drawTag(const GfxRenderer& r, int x, int y, const char* text, bool filled) {
  const int h = r.getLineHeight(SMALL_FONT_ID) + 4;
  const int w = r.getTextWidth(SMALL_FONT_ID, text, EpdFontFamily::BOLD) + 12;
  if (filled) {
    r.fillRoundedRect(x, y, w, h, 6, Color::Black);
  } else {
    r.drawRoundedRect(x, y, w, h, 2, 6, true);
  }
  r.drawText(SMALL_FONT_ID, x + 6, y + 2, text, !filled, EpdFontFamily::BOLD);
  return w;
}

int tagWidth(const GfxRenderer& r, const char* text) {
  return r.getTextWidth(SMALL_FONT_ID, text, EpdFontFamily::BOLD) + 12;
}

void drawButton(const GfxRenderer& r, int x, int y, int w, int h, const char* text, bool primary) {
  if (primary) {
    r.fillRoundedRect(x, y, w, h, RADIUS, Color::Black);
  } else {
    r.drawRoundedRect(x, y, w, h, 2, RADIUS, true);
  }
  drawCentered(r, UI_12_FONT_ID, x, y, w, h, text, !primary, EpdFontFamily::BOLD);
}

// A tick box: filled with a check when done.
void drawCheck(const GfxRenderer& r, int x, int y, int size, bool done, bool thick = false) {
  if (done) {
    r.fillRoundedRect(x, y, size, size, size / 4, Color::Black);
    const int c = x + size / 2;
    const int m = y + size / 2;
    const int u = std::max(2, size / 6);
    r.drawLine(c - 2 * u, m, c - u / 2, m + u + u / 2, 3, false);
    r.drawLine(c - u / 2, m + u + u / 2, c + 2 * u, m - u - u / 2, 3, false);
  } else {
    r.drawRoundedRect(x, y, size, size, thick ? 3 : 2, size / 4, true);
  }
}

bool isGymType(const std::string& t) {
  return std::any_of(std::begin(GYM_TYPES), std::end(GYM_TYPES), [&](const char* g) { return t == g; });
}

// The plan's gym day for a logged Notion type, like the phone's GYM_TYPES.
const training::LiftDay* gymDayForType(const std::string& t) {
  int n = 0;
  const auto* days = training::gymDays(n);
  const char* key = t == "Upper A" || t == "Push" ? "upa" : t == "Arms" ? "arms" : t == "Strength" ? "legs" : "upb";
  for (int i = 0; i < n; ++i) {
    if (std::string(days[i].key) == key) return &days[i];
  }
  return nullptr;
}

int setsOf(const training::LiftDay& d) {
  int n = 0;
  for (int i = 0; i < d.count; ++i) n += d.ex[i].sets;
  return n;
}

std::string shortNames(const training::LiftDay& d) {
  std::string s;
  for (int i = 0; i < d.count; ++i) {
    if (i) s += ", ";
    s += d.ex[i].shortName ? d.ex[i].shortName : d.ex[i].name;
  }
  return s;
}
}  // namespace

TrainingActivity::TrainingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Training", renderer, mappedInput) {}

void TrainingActivity::onEnter() {
  Activity::onEnter();
  training::loadSessions(sessions);
  todayIso = training::today();
  dayIso = todayIso;
  weekStart = todayIso.empty() ? "" : mondayOf(todayIso);
  runWeekStart = weekStart;
  calSel = todayIso.empty() ? 0 : (training::weekday(todayIso) + 6) % 7;
  tab = Tab::Calendar;
  dayOpen = false;
  gymOpen = false;

  // Default lift days: today's plan.
  const int wd = todayIso.empty() ? -1 : training::weekday(todayIso);
  int gymCount = 0;
  const training::LiftDay* gymDays = training::gymDays(gymCount);
  const training::LiftDay* gymDay = &gymDays[0];
  for (int i = 0; i < gymCount; ++i) {
    if (gymDays[i].dow == wd) gymDay = &gymDays[i];
  }
  openLiftDay(gym, gymDay);
  const training::LiftDay* gripToday = training::gripDay(wd);
  openLiftDay(grip, gripToday ? gripToday : training::gripDay(1));
  int hyroxCount = 0;
  openLiftDay(hyrox, training::hyroxDays(hyroxCount));
  loadTicks();

  std::string last;
  if (Storage.exists(LAST_SYNC_PATH)) Storage.readFileToString("TRAIN", LAST_SYNC_PATH, 32, last);
  const time_t lastSync = static_cast<time_t>(std::atoll(last.c_str()));
  if (lastSync > 0) lastSyncText = "Synced " + clockText(lastSync);
  if (!training::hasToken()) {
    syncStatus = "Add Notion key";
  } else {
    const int pending = training::pendingCount();
    if (pending > 0) syncStatus = std::to_string(pending) + " to send";
    syncDue = wifiSaved() && (pending > 0 || sessions.empty() || time(nullptr) - lastSync > RESYNC_SECONDS);
  }
  requestUpdate();
}

void TrainingActivity::onExit() {
  stopWifi();
  Activity::onExit();
  sessions.clear();
  hits.clear();
}

bool TrainingActivity::wifiSaved() const {
  WIFI_STORE.loadFromFile();
  return WIFI_STORE.getCredentialCount() > 0 || !WIFI_STORE.getLastConnectedSsid().empty();
}

std::string TrainingActivity::syncText() const {
  if (syncing) return "Syncing...";
  if (!syncStatus.empty()) return syncStatus;
  return lastSyncText.empty() ? "Tap to sync" : lastSyncText;
}

std::vector<size_t> TrainingActivity::sessionsOn(const std::string& iso) const {
  std::vector<size_t> out;
  for (size_t i = 0; i < sessions.size(); ++i) {
    if (sessions[i].date == iso) out.push_back(i);
  }
  return out;
}

bool TrainingActivity::loggedThisWeek(const char* type, const std::string& iso) const {
  if (iso.empty()) return false;
  const std::string mon = mondayOf(iso);
  const std::string sun = training::addDays(mon, 6);
  for (const auto& s : sessions) {
    if (s.date >= mon && s.date <= sun && s.done() &&
        (s.type == type || (std::string(type) == "Gym" && isGymType(s.type)))) {
      return true;
    }
  }
  return false;
}

std::vector<TrainingActivity::Item> TrainingActivity::itemsOn(const std::string& iso) const {
  std::vector<Item> runs, hyroxes, gyms, grips, others;
  bool gripDone = false;
  for (const size_t i : sessionsOn(iso)) {
    const auto& s = sessions[i];
    Item it{Kind::Other, planTitle(s.title), s.done(), static_cast<int>(i), nullptr};
    if (s.isRun()) {
      it.kind = Kind::Run;
      runs.push_back(it);
    } else if (s.type == "Hyrox") {
      it.kind = Kind::Hyrox;
      if (it.text.empty() || it.text == s.type) it.text = "Mini Hyrox";
      hyroxes.push_back(it);
    } else if (isGymType(s.type)) {
      it.kind = Kind::Gym;
      it.day = gymDayForType(s.type);
      if (it.day) it.text = s.type == "Strength" ? "Legs - Strength" : it.day->label;
      gyms.push_back(it);
    } else if (s.type == "Grip") {
      gripDone = gripDone || s.done();
    } else if (s.type != "Rest" && !(s.type.empty() && it.text.rfind("Rest", 0) == 0)) {
      others.push_back(it);
    }
  }
  const int wd = training::weekday(iso);
  if (gyms.empty()) {
    int n = 0;
    const auto* days = training::gymDays(n);
    for (int i = 0; i < n; ++i) {
      if (days[i].dow != wd) continue;
      Item it{Kind::Gym, days[i].type == std::string("Strength") ? "Legs - Strength" : days[i].label, false, -1,
              &days[i]};
      gyms.push_back(it);
    }
  }
  if (const auto* g = training::gripDay(wd)) {
    std::string names;
    for (int i = 0; i < g->count; ++i) names += (i ? ", " : "") + std::string(g->ex[i].name);
    grips.push_back(Item{Kind::Grip, names, gripDone, -1, g});
  } else if (gripDone) {
    grips.push_back(Item{Kind::Grip, "Grip", true, -1, nullptr});
  }
  std::vector<Item> out;
  for (auto* v : {&runs, &hyroxes, &gyms, &grips, &others}) out.insert(out.end(), v->begin(), v->end());
  return out;
}

void TrainingActivity::setTab(const Tab t) {
  {
    RenderLock lock(*this);
    tab = t;
    if (t == Tab::Gym) gymOpen = false;
    if (t == Tab::Run && syncDue) {
      syncDue = false;
      syncPending = true;
    }
  }
  requestUpdate();
}

// ---- lifts ----

const training::LiftDay* TrainingActivity::chipDay(const Tab t, const int i, int& count) {
  if (t == Tab::Gym) {
    const auto* days = training::gymDays(count);
    return i >= 0 && i < count ? &days[i] : nullptr;
  }
  if (t == Tab::Hyrox) {
    const auto* days = training::hyroxDays(count);
    return i >= 0 && i < count ? &days[i] : nullptr;
  }
  count = 5;  // Mon ... Fri
  return training::gripDay(i + 1);
}

const char* TrainingActivity::liftName(const Lift& l) const {
  return &l == &gym ? "gym" : &l == &grip ? "grip" : "hyrox";
}

void TrainingActivity::openLiftDay(Lift& l, const training::LiftDay* day) {
  l.day = day;
  l.done.assign(day ? day->count : 0, 0);
  l.loads.clear();
  for (int i = 0; day && i < day->count; ++i) l.loads.push_back(training::loadFor(day->ex[i]));
  l.focus = 0;
}

void TrainingActivity::startLift(const Tab t, const training::LiftDay* day) {
  {
    RenderLock lock(*this);
    tab = t;
    dayOpen = false;
    Lift& l = lift();
    if (day && day != l.day) {
      openLiftDay(l, day);
      restUntil = 0;
    }
    if (t == Tab::Gym) gymOpen = true;
  }
  saveTicks();
  requestUpdate();
}

void TrainingActivity::loadTicks() {
  std::string text;
  if (todayIso.empty() || !Storage.exists(TICKS_PATH) || !Storage.readFileToString("TRAIN", TICKS_PATH, 1024, text)) {
    return;
  }
  // tab \t date \t day key \t counts
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    std::vector<std::string> f;
    size_t col = 0;
    while (true) {
      const size_t t = line.find('\t', col);
      f.push_back(line.substr(col, t == std::string::npos ? std::string::npos : t - col));
      if (t == std::string::npos) break;
      col = t + 1;
    }
    if (f.size() < 4 || f[1] != todayIso) continue;
    const Tab t = f[0] == "gym" ? Tab::Gym : f[0] == "grip" ? Tab::Grip : Tab::Hyrox;
    Lift& l = t == Tab::Gym ? gym : t == Tab::Grip ? grip : hyrox;
    const training::LiftDay* day = nullptr;
    int n = 1;
    for (int i = 0; i < n; ++i) {
      const auto* d = chipDay(t, i, n);
      if (d && f[2] == d->key) day = d;
    }
    if (!day) continue;
    openLiftDay(l, day);
    size_t p = 0;
    for (int i = 0; i < day->count && p < f[3].size(); ++i) {
      l.done[i] = std::min(day->ex[i].sets, std::atoi(f[3].c_str() + p));
      const size_t comma = f[3].find(',', p);
      if (comma == std::string::npos) break;
      p = comma + 1;
    }
  }
}

void TrainingActivity::saveTicks() const {
  if (todayIso.empty()) return;
  std::string text;
  for (const Lift* l : {&gym, &grip, &hyrox}) {
    if (!l->day) continue;
    text += std::string(liftName(*l)) + "\t" + todayIso + "\t" + l->day->key + "\t";
    for (size_t i = 0; i < l->done.size(); ++i) text += (i ? "," : "") + std::to_string(l->done[i]);
    text += "\n";
  }
  Storage.ensureDirectoryExists("/.crosspoint/training");
  Storage.writeFile(TICKS_PATH, String(text.c_str()));
}

void TrainingActivity::tick(Lift& l, const int exercise, const int box) {
  if (!l.day || exercise < 0 || exercise >= l.day->count) return;
  const auto& e = l.day->ex[exercise];
  {
    RenderLock lock(*this);
    int& n = l.done[exercise];
    const int target = box < 0 ? std::min(e.sets, n + 1) : box + 1;
    const bool untick = box >= 0 && box < n;
    n = untick ? box : target;
    l.focus = exercise;
    if (!untick && n < e.sets && e.rest > 0) {
      restSeconds = e.rest;
      restUntil = time(nullptr) + e.rest;
    } else {
      restUntil = 0;
    }
  }
  saveTicks();
  requestUpdate();
}

void TrainingActivity::changeLoad(Lift& l, const int exercise, const int dir) {
  if (!l.day || exercise < 0 || exercise >= l.day->count) return;
  const auto& e = l.day->ex[exercise];
  if (e.unit[0] == '\0') return;  // bodyweight
  const bool seconds = e.unit[0] == 's';
  const bool cm = e.unit[0] == 'c';
  const float step = seconds || cm ? 5.0f : (l.loads[exercise] >= 20 ? 2.5f : 1.0f);
  {
    RenderLock lock(*this);
    l.loads[exercise] = std::max(0.0f, l.loads[exercise] + dir * step);
    l.focus = exercise;
  }
  training::setLoad(e, l.loads[exercise]);
  requestUpdate();
}

void TrainingActivity::clearLift(Lift& l) {
  {
    RenderLock lock(*this);
    std::fill(l.done.begin(), l.done.end(), 0);
    l.focus = 0;
    restUntil = 0;
  }
  saveTicks();
  requestUpdate();
}

void TrainingActivity::primary() {
  Lift& l = gym;
  if (!l.day) return;
  if (l.focus >= l.day->count) return saveLift(l);
  if (l.done[l.focus] < l.day->ex[l.focus].sets) return tick(l, l.focus, -1);
  {
    RenderLock lock(*this);
    l.focus++;
    restUntil = 0;
  }
  requestUpdate();
}
void TrainingActivity::saveLift(Lift& l) {
  if (!l.day) return;
  if (todayIso.empty()) {
    {
      RenderLock lock(*this);
      syncStatus = "Set the clock first";
    }
    return requestUpdate();
  }
  std::string log;
  int total = 0;
  int all = 0;
  for (int i = 0; i < l.day->count; ++i) {
    const auto& e = l.day->ex[i];
    all += e.sets;
    total += l.done[i];
    if (l.done[i] == 0) continue;
    if (!log.empty()) log += ". ";
    if (&l == &hyrox) {
      log += std::to_string(i + 1) + ". " + e.name + " " + e.target;
      if (e.unit[0] != '\0') log += " @ " + loadText(l.loads[i], e.unit);
    } else {
      log += std::string(e.name) + " " + std::to_string(l.done[i]) + "/" + std::to_string(e.sets);
      if (e.unit[0] != '\0') log += " @ " + loadText(l.loads[i], e.unit);
    }
  }
  if (total == 0) {
    {
      RenderLock lock(*this);
      syncStatus = "Tick a set first";
    }
    return requestUpdate();
  }
  const bool isGym = &l == &gym;
  const bool isHyrox = &l == &hyrox;
  const int week = training::weekOf(todayIso);
  training::Change c;
  c.status = (!isGym && !isHyrox) || total == all ? "Done" : "Modified";

  // Legs fills in the week's planned Strength session, like the phone app.
  training::Session* planned = nullptr;
  // Hyrox fills in today's planned Hyrox session the same way.
  if (isGym && std::string(l.day->key) == "legs") {
    for (auto& s : sessions) {
      if (s.type == "Strength" && s.week == week && !s.done() && !s.id.empty()) planned = &s;
    }
  }
  if (isHyrox) {
    for (auto& s : sessions) {
      if (s.type == "Hyrox" && s.date == todayIso && !s.done() && !s.id.empty()) planned = &s;
    }
  }
  const std::string targetText = isHyrox ? std::to_string(total) + "/" + std::to_string(all) + " rounds"
                                 : isGym ? std::to_string(total) + "/" + std::to_string(all) + " sets"
                                         : std::to_string(total) + " sets";
  if (planned && isHyrox) {
    c.pageId = planned->id;
    c.title = "Mini Hyrox " + std::string(l.day->label) + " " + shortDay(todayIso);
    c.workout = log;
    c.targetPace = targetText;
    planned->title = c.title;
    planned->status = c.status;
    planned->workout = c.workout;
    planned->targetPace = c.targetPace;
  } else if (planned) {
    c.pageId = planned->id;
    c.notes = "Log: " + log;
    planned->status = c.status;
    planned->notes = c.notes;
  } else {
    c.title = (isGym     ? std::string(l.day->label)
               : isHyrox ? "Mini Hyrox " + std::string(l.day->label)
                         : std::string("Grip")) +
              " " + shortDay(todayIso);
    c.date = todayIso;
    c.type = l.day->type;
    c.week = week;
    c.workout = log;
    c.targetPace = targetText;
    training::Session s;
    s.date = c.date;
    s.title = c.title;
    s.type = c.type;
    s.status = c.status;
    s.workout = c.workout;
    s.targetPace = c.targetPace;
    s.week = week;
    sessions.push_back(std::move(s));
  }
  if (!training::queue(c)) {
    {
      RenderLock lock(*this);
      syncStatus = "Could not save on the card";
    }
    return requestUpdate();
  }
  training::saveSessions(sessions);
  {
    RenderLock lock(*this);
    std::fill(l.done.begin(), l.done.end(), 0);
    l.focus = 0;
    restUntil = 0;
    if (&l == &gym) gymOpen = false;
    syncStatus = "Saved";
  }
  saveTicks();
  if (wifiSaved() && training::hasToken()) return startSync();
  requestUpdate();
}

// ---- sync ----

void TrainingActivity::startSync() {
  if (!training::hasToken()) {
    {
      RenderLock lock(*this);
      syncStatus = "Add Notion key";
    }
    requestUpdate();
    return;
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           swallowBack = true;
                           if (result.isCancelled) {
                             stopWifi();
                             const int pending = training::pendingCount();
                             syncStatus = pending > 0 ? std::to_string(pending) + " to send" : "";
                             requestUpdate();
                             return;
                           }
                           runSync();
                         });
}

void TrainingActivity::runSync() {
  {
    RenderLock lock(*this);
    syncing = true;
    syncStatus = "Syncing...";
  }
  requestUpdateAndWait();
  std::vector<training::Session> fresh = sessions;
  std::string message;
  const bool ok = training::sync(fresh, message);
  stopWifi();
  if (ok) {
    const time_t now = time(nullptr);
    Storage.writeFile(LAST_SYNC_PATH, String(std::to_string(static_cast<long long>(now)).c_str()));
  }
  {
    RenderLock lock(*this);
    syncing = false;
    if (ok) {
      sessions = std::move(fresh);
      lastSyncText = "Synced " + clockText(time(nullptr));
    } else {
      training::loadSessions(sessions);
    }
    syncStatus = ok ? "" : message;
  }
  requestUpdate();
}

void TrainingActivity::stopWifi() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
}

void TrainingActivity::openLog(const size_t sessionIndex) {
  if (sessionIndex >= sessions.size()) return;
  if (sessions[sessionIndex].id.empty()) {
    {
      RenderLock lock(*this);
      syncStatus = "Waiting for sync";
    }
    return requestUpdate();
  }
  auto screen = makeUniqueNoThrow<TrainingLogActivity>(renderer, mappedInput, sessions[sessionIndex]);
  if (!screen) {
    LOG_ERR("TRAIN", "OOM: log screen");
    return;
  }
  startActivityForResult(std::move(screen), [this](const ActivityResult& result) {
    swallowBack = true;
    if (result.isCancelled) return requestUpdate();
    training::loadSessions(sessions);
    syncStatus = "Saved";
    if (wifiSaved()) {
      syncPending = true;
    }
    requestUpdate();
  });
}

// ---- input ----

void TrainingActivity::loop() {
  using B = MappedInputManager::Button;
  if (syncPending) {
    syncPending = false;
    startSync();
    return;
  }
  if (syncing) return;
  // The Back that closed a child screen must not close this one too.
  if (swallowBack) {
    if (mappedInput.wasReleased(B::Back)) {
      swallowBack = false;
      return;
    }
    if (!mappedInput.isPressed(B::Back)) swallowBack = false;
  }
  const auto update = [&](auto&& f) {
    {
      RenderLock lock(*this);
      f();
    }
    requestUpdate();
  };
  if (mappedInput.wasReleased(B::Back)) {
    if (tab == Tab::Gym && gymOpen) return update([&] { gymOpen = false; });
    if (tab == Tab::Calendar && dayOpen) return update([&] { dayOpen = false; });
    finish();
    return;
  }
  if (mappedInput.wasReleased(B::Right)) return setTab(static_cast<Tab>((static_cast<int>(tab) + 1) % TAB_COUNT));
  if (mappedInput.wasReleased(B::Left)) {
    return setTab(static_cast<Tab>((static_cast<int>(tab) + TAB_COUNT - 1) % TAB_COUNT));
  }
  const bool down = mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::PageForward);
  const bool up = mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::PageBack);
  const bool confirm = mappedInput.wasReleased(B::Confirm);

  switch (tab) {
    case Tab::Calendar:
      if (!dayOpen && (down || up) && !weekStart.empty()) {
        return update([&] {
          calSel += down ? 1 : -1;
          if (calSel > 6) weekStart = training::addDays(weekStart, 7), calSel = 0;
          if (calSel < 0) weekStart = training::addDays(weekStart, -7), calSel = 6;
        });
      }
      if (!dayOpen && confirm && !weekStart.empty()) {
        return update([&] {
          dayIso = training::addDays(weekStart, calSel);
          dayOpen = true;
        });
      }
      break;
    case Tab::Run:
      if ((down || up) && !runWeekStart.empty()) {
        return update([&] { runWeekStart = training::addDays(runWeekStart, down ? 7 : -7); });
      }
      break;
    case Tab::Gym:
      if (!gymOpen && confirm) return startLift(Tab::Gym, gym.day);
      if (gymOpen && gym.day && (down || up)) {
        return update([&] {
          gym.focus = std::max(0, std::min(gym.day->count, gym.focus + (down ? 1 : -1)));
          restUntil = 0;
        });
      }
      if (gymOpen && confirm) return primary();
      break;
    case Tab::Grip:
    case Tab::Hyrox: {
      Lift& l = lift();
      const int count = l.day ? l.day->count : 0;
      if ((down || up) && count > 0) {
        return update([&] { l.focus = (l.focus + (down ? 1 : count - 1)) % count; });
      }
      if (confirm && count > 0) {
        if (tab == Tab::Hyrox) return tick(l, l.focus, l.done[l.focus] > 0 ? 0 : -1);
        return tick(l, l.focus, -1);
      }
      break;
    }
  }

  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return;
  const auto& m = UITheme::getInstance().getMetrics();
  if (y < m.topPadding + m.headerHeight && x > renderer.getScreenWidth() / 2) return startSync();
  const bool barShown = !(tab == Tab::Gym && gymOpen);
  if (barShown && y >= tabBarTop() && y < tabBarTop() + TAB_BAR_HEIGHT) {
    const int t = x * TAB_COUNT / std::max(1, renderer.getScreenWidth());
    return setTab(static_cast<Tab>(std::min(t, TAB_COUNT - 1)));
  }
  for (const auto& h : hits) {
    if (x < h.x0 || x >= h.x1 || y < h.y0 || y >= h.y1) continue;
    switch (h.kind) {
      case DayRow:
        return update([&] {
          calSel = h.a;
          dayIso = training::addDays(weekStart, h.a);
          dayOpen = true;
        });
      case PrevWeek:
      case NextWeek: {
        const int d = h.kind == NextWeek ? 7 : -7;
        return update([&] {
          if (tab == Tab::Run) {
            runWeekStart = training::addDays(runWeekStart, d);
          } else {
            weekStart = training::addDays(weekStart, d);
          }
        });
      }
      case CloseDay:
        return update([&] { dayOpen = false; });
      case OpenLog:
        return openLog(static_cast<size_t>(h.a));
      case StartGym: {
        int n = 0;
        const auto* days = training::gymDays(n);
        return startLift(Tab::Gym, h.a >= 0 && h.a < n ? &days[h.a] : gym.day);
      }
      case StartGrip:
        return startLift(Tab::Grip, training::gripDay(h.a));
      case GoTab:
        return update([&] {
          dayOpen = false;
          tab = static_cast<Tab>(h.a);
          if (tab == Tab::Gym) gymOpen = false;
        });
      case Step:
        return update([&] {
          gym.focus = h.a;
          restUntil = 0;
        });
      case SetBox:
        return tick(lift(), h.a, h.b);
      case LoadDown:
        return changeLoad(lift(), h.a, -1);
      case LoadUp:
        return changeLoad(lift(), h.a, 1);
      case Primary:
        return primary();
      case SaveLift:
        return saveLift(lift());
      case ClearLift:
        return clearLift(lift());
      case Chip: {
        Lift& l = lift();
        int n = 0;
        const training::LiftDay* day = chipDay(tab, h.a, n);
        if (day && day != l.day) {
          {
            RenderLock lock(*this);
            openLiftDay(l, day);
            restUntil = 0;
          }
          saveTicks();
        }
        return requestUpdate();
      }
      case HxToggle:
        return tick(hyrox, h.a, hyrox.done[h.a] > 0 ? 0 : -1);
      case HxFocus:
        return update([&] {
          hyrox.focus = h.a;
          hxEdit = true;
        });
      case CloseGym:
        return update([&] { gymOpen = false; });
      case Sync:
        return startSync();
      default:
        return;
    }
  }
}

// ---- render ----

int TrainingActivity::contentTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight;
}

int TrainingActivity::tabBarTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - m.buttonHintsHeight - TAB_BAR_HEIGHT;
}

void TrainingActivity::render(RenderLock&&) {
  renderer.clearScreen();
  hits.clear();
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const std::string right = syncText();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, w, m.headerHeight}, "TRAINING.", right.c_str());
  const int y = contentTop();
  switch (tab) {
    case Tab::Calendar:
      dayOpen ? renderDay(y) : renderCalendar(y);
      break;
    case Tab::Run:
      renderRun(y);
      break;
    case Tab::Gym:
      gymOpen ? renderGymStep(y) : renderGymList(y);
      break;
    case Tab::Grip:
      renderGrip(y);
      break;
    case Tab::Hyrox:
      renderHyrox(y);
      break;
  }
  if (!(tab == Tab::Gym && gymOpen)) renderTabBar();
  renderer.displayBuffer();
}

// [<]  title / sub  [>]
int TrainingActivity::renderWeekNav(const int y, const std::string& monday, const std::string& sub) {
  const int w = renderer.getScreenWidth();
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawIcon(TrainChevLIcon, 18, y + (NAV_H - ICON) / 2, ICON);
  renderer.drawIcon(TrainChevRIcon, w - 18 - ICON, y + (NAV_H - ICON) / 2, ICON);
  hits.push_back({0, y, 70, y + NAV_H, PrevWeek, 0, 0});
  hits.push_back({w - 70, y, w, y + NAV_H, NextWeek, 0, 0});
  const std::string title = monday.empty() ? "Set the clock" : weekRange(monday);
  const int blockH = bodyH + (sub.empty() ? 0 : smallH);
  int ty = y + (NAV_H - blockH) / 2;
  drawCentered(renderer, UI_12_FONT_ID, 0, ty, w, bodyH, title.c_str(), true, EpdFontFamily::BOLD);
  if (!sub.empty()) {
    ty += bodyH;
    drawCentered(renderer, SMALL_FONT_ID, 0, ty, w, smallH, sub.c_str(), true);
  }
  renderer.drawLine(0, y + NAV_H, w - 1, y + NAV_H, 2, true);
  return y + NAV_H + 2;
}

void TrainingActivity::renderCalendar(int y) {
  const int w = renderer.getScreenWidth();
  const int pad = 16;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  if (weekStart.empty()) {
    renderer.drawText(UI_12_FONT_ID, pad, y + GAP, "Set the clock to see the plan.");
    return;
  }
  // Week totals for the nav line.
  float plannedKm = 0, doneKm = 0;
  int items = 0, done = 0;
  std::vector<std::vector<Item>> week;
  for (int i = 0; i < 7; ++i) {
    week.push_back(itemsOn(training::addDays(weekStart, i)));
    for (const auto& it : week.back()) {
      items++;
      done += it.done ? 1 : 0;
      if (it.kind == Kind::Run && it.session >= 0) {
        plannedKm += sessions[it.session].plannedKm;
        doneKm += sessions[it.session].actualKm;
      }
    }
  }
  std::string sub = std::to_string(done) + " of " + std::to_string(items) + " done";
  if (plannedKm > 0) sub = "Run " + num(doneKm) + " of " + num(plannedKm) + " km  -  " + sub;
  y = renderWeekNav(y, weekStart, sub);

  const int rowH = (tabBarTop() - y) / 7;
  const int blockW = 54;
  const int blockH = std::min(64, rowH - 8);
  const int tagW = renderer.getTextWidth(SMALL_FONT_ID, "HYROX", EpdFontFamily::BOLD) + 10;
  const int lineH = std::max(bodyH, 22);
  for (int i = 0; i < 7; ++i) {
    const std::string iso = training::addDays(weekStart, i);
    const bool isToday = iso == todayIso;
    const int ry = y + i * rowH;
    // Day block.
    const int bx = pad;
    const int by = ry + (rowH - blockH) / 2;
    if (isToday) {
      renderer.fillRoundedRect(bx, by, blockW, blockH, RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(bx, by, blockW, blockH, 2, RADIUS, true);
    }
    const int dh = renderer.getLineHeight(NOTOSANS_16_FONT_ID);
    const int top = by + (blockH - smallH - dh) / 2 + 2;
    drawCentered(renderer, SMALL_FONT_ID, bx, top, blockW, smallH, DAYS[training::weekday(iso)], !isToday,
                 EpdFontFamily::BOLD);
    drawCentered(renderer, NOTOSANS_16_FONT_ID, bx, top + smallH, blockW, dh, std::to_string(dayOfMonth(iso)).c_str(),
                 !isToday, EpdFontFamily::BOLD);
    if (i == calSel && !isToday) renderer.fillRect(2, by + 8, 5, blockH - 16, true);

    // Items, up to three lines.
    const auto& list = week[i];
    const int tx = bx + blockW + 14;
    const int textX = tx + 16 + 8 + tagW;
    const int textW = w - 40 - textX;
    const int shown = std::min<int>(3, static_cast<int>(list.size()));
    int ly = ry + (rowH - std::max(1, shown) * lineH) / 2;
    if (list.empty()) renderer.drawText(UI_12_FONT_ID, tx, ly + (lineH - bodyH) / 2, "Rest");
    for (int k = 0; k < shown; ++k) {
      const auto& it = list[k];
      const char* tag = it.kind == Kind::Run     ? "RUN"
                        : it.kind == Kind::Hyrox ? "HYROX"
                        : it.kind == Kind::Gym   ? "GYM"
                        : it.kind == Kind::Grip  ? "GRIP"
                                                 : "PLAN";
      drawCheck(renderer, tx, ly + (lineH - 16) / 2, 16, it.done);
      renderer.drawText(SMALL_FONT_ID, tx + 24, ly + (lineH - smallH) / 2, tag, true, EpdFontFamily::BOLD);
      std::string text = it.text;
      if (k == 2 && list.size() > 3) text = "+" + std::to_string(list.size() - 2) + " more";
      const auto style = it.done ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
      const std::string fit = renderer.truncatedText(UI_12_FONT_ID, text.c_str(), textW, style);
      renderer.drawText(UI_12_FONT_ID, textX, ly + (lineH - bodyH) / 2, fit.c_str(), true, style);
      ly += lineH;
    }
    renderer.drawIcon(TrainChevRIcon, w - 14 - ICON, ry + (rowH - ICON) / 2, ICON);
    if (i < 6) renderer.drawLine(pad, ry + rowH, w - pad, ry + rowH, true);
    hits.push_back({0, ry, w, ry + rowH, DayRow, i, 0});
  }
}

void TrainingActivity::renderDay(int y) {
  const int w = renderer.getScreenWidth();
  const int pad = 16;
  const int inset = 14;
  const int width = w - 2 * pad;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  const int bigH = renderer.getLineHeight(NOTOSANS_16_FONT_ID);
  const int bottom = tabBarTop() - GAP;

  // "< Thursday 19 Nov  TODAY"
  const int headH = 52;
  renderer.drawIcon(TrainChevLIcon, pad - 4, y + (headH - ICON) / 2, ICON);
  const int wd = training::weekday(dayIso);
  const std::string title =
      std::string(wd >= 0 ? DAYS_LONG[wd] : "") + " " + std::to_string(dayOfMonth(dayIso)) + " " + monthName(dayIso);
  const int th = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  renderer.drawText(NOTOSANS_18_FONT_ID, pad + 26, y + (headH - th) / 2, title.c_str(), true, EpdFontFamily::BOLD);
  if (dayIso == todayIso)
    drawTag(renderer, w - pad - tagWidth(renderer, "TODAY"), y + (headH - smallH) / 2, "TODAY", true);
  hits.push_back({0, y, w, y + headH, CloseDay, 0, 0});
  y += headH;
  renderer.drawLine(0, y, w - 1, y, 2, true);
  y += GAP;

  const auto items = itemsOn(dayIso);
  bool hasGym = false;
  for (const auto& it : items) {
    hasGym = hasGym || it.kind == Kind::Gym;
    // Measure the card first so it fits its content.
    std::vector<std::string> body;
    std::string tag, chip, button;
    int hitKind = 0, hitA = 0;
    bool stats = false;
    const training::Session* s = it.session >= 0 ? &sessions[it.session] : nullptr;
    if (it.kind == Kind::Run && s) {
      tag = "RUN  " + upper(s->type);
      chip = s->id.empty() ? "SENDING" : s->status.empty() ? "PLANNED" : upper(s->status);
      stats = s->plannedKm > 0 || !s->targetPace.empty();
      if (!s->workout.empty()) body = renderer.wrappedText(SMALL_FONT_ID, s->workout.c_str(), width - 2 * inset, 3);
      if (s->done()) {
        std::string r = s->actualKm > 0 ? num(s->actualKm) + " km" : s->status;
        if (!s->actualPace.empty()) r += ", " + s->actualPace + " /km";
        if (s->hr > 0) r += ", HR " + std::to_string(s->hr);
        body.push_back("Logged: " + r);
      }
      button = s->done() ? "Edit log" : "Log run";
      hitKind = OpenLog;
      hitA = it.session;
    } else if (it.kind == Kind::Gym && it.day) {
      tag = "GYM";
      chip = it.done ? "DONE" : "";
      body.push_back(std::to_string(it.day->count) + " exercises, " + std::to_string(setsOf(*it.day)) + " sets");
      for (const auto& l : renderer.wrappedText(SMALL_FONT_ID, shortNames(*it.day).c_str(), width - 2 * inset, 2))
        body.push_back(l);
      button = std::string("Start ") + it.day->label;
      int n = 0;
      const auto* days = training::gymDays(n);
      hitKind = StartGym;
      hitA = static_cast<int>(it.day - days);
    } else if (it.kind == Kind::Grip && it.day) {
      tag = "GRIP";
      chip = it.done ? "DONE" : "";
      for (int i = 0; i < it.day->count; ++i) {
        const auto& e = it.day->ex[i];
        body.push_back(std::string(e.name) + "  " + e.target + ", " + loadText(training::loadFor(e), e.unit));
      }
      button = "Start grip";
      hitKind = StartGrip;
      hitA = wd;
    } else if (it.kind == Kind::Hyrox) {
      tag = "HYROX";
      chip = it.done ? "DONE" : s && !s->status.empty() ? upper(s->status) : "";
      button = "Open Hyrox";
      hitKind = GoTab;
      hitA = static_cast<int>(Tab::Hyrox);
    } else {
      tag = s ? upper(s->type) : "PLAN";
      chip = it.done ? "DONE" : "";
    }
    const std::string heading = it.kind == Kind::Grip ? "" : it.text;
    const auto headLines = heading.empty() ? std::vector<std::string>{}
                                           : renderer.wrappedText(NOTOSANS_16_FONT_ID, heading.c_str(),
                                                                  width - 2 * inset, 2, EpdFontFamily::BOLD);
    const int statsH = stats ? 58 : 0;
    const int cardH = inset + smallH + 8 + static_cast<int>(headLines.size()) * bigH + (stats ? statsH + 8 : 0) +
                      static_cast<int>(body.size()) * (smallH + 2) + (button.empty() ? 0 : 10 + BTN_H) + inset;
    if (y + cardH > bottom) {
      renderer.drawText(SMALL_FONT_ID, pad, y, "More in the Run, Gym and Grip tabs.");
      return;
    }
    renderer.drawRoundedRect(pad, y, width, cardH, 2, 14, true);
    int cy = y + inset;
    renderer.drawText(SMALL_FONT_ID, pad + inset, cy, tag.c_str(), true, EpdFontFamily::BOLD);
    if (!chip.empty())
      drawTag(renderer, pad + width - inset - tagWidth(renderer, chip.c_str()), cy - 2, chip.c_str(), it.done);
    cy += smallH + 8;
    for (const auto& l : headLines) {
      renderer.drawText(NOTOSANS_16_FONT_ID, pad + inset, cy, l.c_str(), true, EpdFontFamily::BOLD);
      cy += bigH;
    }
    if (stats) {
      const int cw = (width - 2 * inset) / 2;
      renderer.drawRoundedRect(pad + inset, cy, 2 * cw, statsH, 1, 10, true);
      renderer.drawLine(pad + inset + cw, cy, pad + inset + cw, cy + statsH - 1, true);
      const std::string vals[2] = {s->plannedKm > 0 ? num(s->plannedKm) + " km" : "-",
                                   s->targetPace.empty() ? "-" : s->targetPace};
      const char* labs[2] = {"Distance", "Pace"};
      for (int k = 0; k < 2; ++k) {
        const int cx = pad + inset + k * cw + 10;
        renderer.drawText(SMALL_FONT_ID, cx, cy + 6, labs[k]);
        renderer.drawText(UI_12_FONT_ID, cx, cy + 8 + smallH, vals[k].c_str(), true, EpdFontFamily::BOLD);
      }
      cy += statsH + 8;
    }
    for (const auto& l : body) {
      renderer.drawText(SMALL_FONT_ID, pad + inset, cy, l.c_str());
      cy += smallH + 2;
    }
    if (!button.empty()) {
      cy += 10;
      drawButton(renderer, pad + inset, cy, width - 2 * inset, BTN_H, button.c_str(), !it.done);
      hits.push_back({pad + inset, cy, pad + width - inset, cy + BTN_H, hitKind, hitA, 0});
    }
    y += cardH + GAP;
  }
  if (items.empty()) {
    renderer.drawText(NOTOSANS_16_FONT_ID, pad, y, "Rest day", true, EpdFontFamily::BOLD);
    y += bigH + GAP;
  }
  // Make one up.
  if (y + smallH + 8 + BTN_H <= bottom) {
    renderer.drawText(SMALL_FONT_ID, pad, y, "NOT ON THE PLAN", true, EpdFontFamily::BOLD);
    y += smallH + 8;
    const int bw = (width - 10) / 2;
    drawButton(renderer, pad, y, bw, BTN_H, hasGym ? "Mini Hyrox" : "Gym session", false);
    hits.push_back({pad, y, pad + bw, y + BTN_H, GoTab, static_cast<int>(hasGym ? Tab::Hyrox : Tab::Gym), 0});
    drawButton(renderer, pad + bw + 10, y, bw, BTN_H, "Grip", false);
    hits.push_back({pad + bw + 10, y, pad + width, y + BTN_H, GoTab, static_cast<int>(Tab::Grip), 0});
  }
}

void TrainingActivity::renderRun(int y) {
  const int w = renderer.getScreenWidth();
  const int pad = 16;
  const int width = w - 2 * pad;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  if (runWeekStart.empty()) {
    renderer.drawText(UI_12_FONT_ID, pad, y + GAP, "Set the clock to see the plan.");
    return;
  }
  const int week = training::weekOf(runWeekStart);
  y = renderWeekNav(y, runWeekStart, week > 0 ? "Week " + std::to_string(week) : "");
  const std::string sun = training::addDays(runWeekStart, 6);
  std::vector<size_t> runs;
  float planned = 0, done = 0;
  int doneRuns = 0;
  for (size_t i = 0; i < sessions.size(); ++i) {
    const auto& s = sessions[i];
    if (!s.isRun() || s.date < runWeekStart || s.date > sun) continue;
    runs.push_back(i);
    planned += s.plannedKm;
    done += s.actualKm;
    doneRuns += s.done() ? 1 : 0;
  }
  std::sort(runs.begin(), runs.end(), [&](size_t a, size_t b) { return sessions[a].date < sessions[b].date; });

  // Done, Planned, Runs.
  y += GAP;
  const int boxH = smallH + renderer.getLineHeight(NOTOSANS_16_FONT_ID) + 18;
  const int cw = width / 3;
  renderer.drawRoundedRect(pad, y, width, boxH, 2, RADIUS, true);
  const std::string vals[3] = {num(done) + " km", num(planned) + " km",
                               std::to_string(doneRuns) + "/" + std::to_string(runs.size())};
  const char* labs[3] = {"Done", "Planned", "Runs"};
  for (int k = 0; k < 3; ++k) {
    const int cx = pad + k * cw;
    if (k) renderer.drawLine(cx, y, cx, y + boxH - 1, 2, true);
    renderer.drawText(SMALL_FONT_ID, cx + 12, y + 6, labs[k]);
    renderer.drawText(NOTOSANS_16_FONT_ID, cx + 12, y + 8 + smallH, vals[k].c_str(), true, EpdFontFamily::BOLD);
  }
  y += boxH + GAP;

  const int rowH = 58;
  renderer.drawLine(0, y, w - 1, y, true);
  if (runs.empty()) {
    renderer.drawText(UI_12_FONT_ID, pad, y + GAP, "No runs this week.");
    y += bodyH + 2 * GAP;
  }
  for (const size_t i : runs) {
    const auto& s = sessions[i];
    if (y + rowH > tabBarTop() - 150) break;  // keep room for the mileage table
    drawCheck(renderer, pad, y + (rowH - 24) / 2, 24, s.done());
    std::string right;
    if (s.done()) {
      right = s.actualKm > 0 ? num(s.actualKm) + " km" : s.status;
      if (!s.actualPace.empty()) right += "  " + s.actualPace;
    }
    const int rightW = right.empty() ? tagWidth(renderer, s.status == "Missed" ? "MISSED" : "PLANNED")
                                     : renderer.getTextWidth(SMALL_FONT_ID, right.c_str(), EpdFontFamily::BOLD);
    const int tx = pad + 36;
    const int tw = w - pad - rightW - 10 - tx;
    const std::string t = renderer.truncatedText(UI_12_FONT_ID, planTitle(s.title).c_str(), tw, EpdFontFamily::BOLD);
    const int top = y + (rowH - bodyH - smallH) / 2;
    renderer.drawText(UI_12_FONT_ID, tx, top, t.c_str(), true, EpdFontFamily::BOLD);
    const int wd = training::weekday(s.date);
    std::string meta = std::string(wd >= 0 ? DAYS[wd] : "") + " " + std::to_string(dayOfMonth(s.date));
    if (s.plannedKm > 0) meta += "  -  " + num(s.plannedKm) + " km";
    if (!s.targetPace.empty()) meta += "  -  " + s.targetPace;
    renderer.drawText(SMALL_FONT_ID, tx, top + bodyH, renderer.truncatedText(SMALL_FONT_ID, meta.c_str(), tw).c_str());
    if (right.empty()) {
      drawTag(renderer, w - pad - rightW, y + (rowH - smallH - 4) / 2, s.status == "Missed" ? "MISSED" : "PLANNED",
              false);
    } else {
      renderer.drawText(SMALL_FONT_ID, w - pad - rightW, y + (rowH - smallH) / 2, right.c_str(), true,
                        EpdFontFamily::BOLD);
    }
    hits.push_back({0, y, w, y + rowH, OpenLog, static_cast<int>(i), 0});
    y += rowH;
    renderer.drawLine(0, y, w - 1, y, true);
  }

  // Weekly mileage of the weeks on the device.
  std::vector<int> weeks;
  for (const auto& s : sessions) {
    if (s.isRun() && s.week > 0 && std::find(weeks.begin(), weeks.end(), s.week) == weeks.end())
      weeks.push_back(s.week);
  }
  std::sort(weeks.begin(), weeks.end());
  y += GAP;
  renderer.drawText(SMALL_FONT_ID, pad, y, "WEEKLY MILEAGE", true, EpdFontFamily::BOLD);
  y += smallH + 6;
  const int lineH = 32;
  const int rows = std::min<int>(static_cast<int>(weeks.size()), (tabBarTop() - GAP - y) / lineH - 1);
  if (rows <= 0) return;
  const int tableH = (rows + 1) * lineH;
  renderer.drawRoundedRect(pad, y, width, tableH, 2, RADIUS, true);
  const int cols[4] = {pad + 12, pad + width * 35 / 100, pad + width * 62 / 100, pad + width - 12};
  const auto row = [&](int ry, const std::string& a, const std::string& b, const std::string& c, const std::string& d,
                       bool bold, bool white) {
    const auto st = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int ty = ry + (lineH - smallH) / 2;
    renderer.drawText(SMALL_FONT_ID, cols[0], ty, a.c_str(), !white, st);
    renderer.drawText(SMALL_FONT_ID, cols[1], ty, b.c_str(), !white, st);
    renderer.drawText(SMALL_FONT_ID, cols[2], ty, c.c_str(), !white, st);
    renderer.drawText(SMALL_FONT_ID, cols[3] - renderer.getTextWidth(SMALL_FONT_ID, d.c_str(), st), ty, d.c_str(),
                      !white, st);
  };
  row(y, "Week", "Plan", "Actual", "Runs", true, false);
  renderer.drawLine(pad, y + lineH, pad + width - 1, y + lineH, true);
  // Show the weeks around the one in view.
  int first = 0;
  for (int k = 0; k < static_cast<int>(weeks.size()); ++k) {
    if (weeks[k] == week) first = std::max(0, std::min(k - rows / 2, static_cast<int>(weeks.size()) - rows));
  }
  for (int k = 0; k < rows; ++k) {
    const int wk = weeks[first + k];
    float p = 0, a = 0;
    int n = 0, d = 0;
    for (const auto& s : sessions) {
      if (!s.isRun() || s.week != wk) continue;
      p += s.plannedKm;
      a += s.actualKm;
      n++;
      d += s.done() ? 1 : 0;
    }
    const int ry = y + (k + 1) * lineH;
    const bool on = wk == week;
    if (on) renderer.fillRect(pad + 2, ry, width - 4, lineH, true);
    row(ry, "W" + std::to_string(wk), num(p), a > 0 ? num(a) : "-", std::to_string(d) + "/" + std::to_string(n), on,
        on);
  }
}

void TrainingActivity::renderGymList(int y) {
  const int w = renderer.getScreenWidth();
  const int pad = 16;
  const int inset = 14;
  const int width = w - 2 * pad;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bigH = renderer.getLineHeight(NOTOSANS_16_FONT_ID);
  y += GAP;
  int n = 0;
  const auto* days = training::gymDays(n);
  const int wd = todayIso.empty() ? -1 : training::weekday(todayIso);
  for (int i = 0; i < n; ++i) {
    const auto& d = days[i];
    const auto names = renderer.wrappedText(SMALL_FONT_ID, shortNames(d).c_str(), width - 2 * inset, 2);
    const int cardH = inset + bigH + 4 + smallH + 4 + static_cast<int>(names.size()) * smallH + inset;
    if (y + cardH > tabBarTop() - GAP) break;
    const bool today = d.dow == wd;
    const bool done =
        loggedThisWeek(d.type, todayIso) && (std::string(d.type) != "Strength" || loggedThisWeek("Strength", todayIso));
    renderer.drawRoundedRect(pad, y, width, cardH, today ? 3 : 2, 14, true);
    int cy = y + inset;
    renderer.drawText(NOTOSANS_16_FONT_ID, pad + inset, cy, d.label, true, EpdFontFamily::BOLD);
    if (today || done) {
      const char* t = today ? "TODAY" : "DONE";
      drawTag(renderer, pad + width - inset - tagWidth(renderer, t), cy + 2, t, today);
    }
    cy += bigH + 4;
    const std::string meta = std::string(DAYS_LONG[d.dow]) + "  -  " + std::to_string(d.count) + " exercises, " +
                             std::to_string(setsOf(d)) + " sets";
    renderer.drawText(SMALL_FONT_ID, pad + inset, cy, meta.c_str(), true, EpdFontFamily::BOLD);
    cy += smallH + 4;
    for (const auto& l : names) {
      renderer.drawText(SMALL_FONT_ID, pad + inset, cy, l.c_str());
      cy += smallH;
    }
    hits.push_back({pad, y, pad + width, y + cardH, StartGym, i, 0});
    y += cardH + GAP;
  }
  if (y + 2 * smallH < tabBarTop()) {
    renderer.drawText(SMALL_FONT_ID, pad, y, "Tap a session to start it. It is saved to today.");
  }
}

int TrainingActivity::renderSetRow(const int x, const int y, const int width, const Lift& l, const int exercise,
                                   const int height) {
  const auto& e = l.day->ex[exercise];
  const int gap = 8;
  const int bw = (width - (e.sets - 1) * gap) / e.sets;
  for (int s = 0; s < e.sets; ++s) {
    const int bx = x + s * (bw + gap);
    const bool done = s < l.done[exercise];
    const bool next = s == l.done[exercise];
    const std::string label = bw >= 70 ? "Set " + std::to_string(s + 1) : std::to_string(s + 1);
    if (done) {
      renderer.fillRoundedRect(bx, y, bw, height, RADIUS, Color::Black);
      drawCentered(renderer, UI_12_FONT_ID, bx, y, bw, height, label.c_str(), false, EpdFontFamily::BOLD);
    } else {
      renderer.drawRoundedRect(bx, y, bw, height, next ? 3 : 1, RADIUS, true);
      drawCentered(renderer, UI_12_FONT_ID, bx, y, bw, height, label.c_str(), true,
                   next ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }
    hits.push_back({bx - gap / 2, y - 4, bx + bw + gap / 2, y + height + 4, SetBox, exercise, s});
  }
  return height;
}

int TrainingActivity::renderLoadRow(const int x, const int y, const int width, const Lift& l, const int exercise,
                                    const char* label) {
  const auto& e = l.day->ex[exercise];
  const int h = 56;
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawRoundedRect(x, y, width, h, 2, RADIUS, true);
  renderer.drawText(UI_12_FONT_ID, x + 14, y + (h - bodyH) / 2, label, true, EpdFontFamily::BOLD);
  if (e.unit[0] == '\0') {
    const char* bw = "Bodyweight";
    renderer.drawText(UI_12_FONT_ID, x + width - 14 - renderer.getTextWidth(UI_12_FONT_ID, bw), y + (h - bodyH) / 2,
                      bw);
    return h;
  }
  const int b = 44;
  const int valW = 112;
  const int plusX = x + width - 6 - b;
  const int minusX = plusX - valW - b;
  renderer.drawRoundedRect(minusX, y + (h - b) / 2, b, b, 2, 10, true);
  drawCentered(renderer, NOTOSANS_16_FONT_ID, minusX, y + (h - b) / 2, b, b, "-", true, EpdFontFamily::BOLD);
  renderer.drawRoundedRect(plusX, y + (h - b) / 2, b, b, 2, 10, true);
  drawCentered(renderer, NOTOSANS_16_FONT_ID, plusX, y + (h - b) / 2, b, b, "+", true, EpdFontFamily::BOLD);
  const std::string v = loadText(l.loads[exercise], e.unit);
  drawCentered(renderer, NOTOSANS_16_FONT_ID, minusX + b, y, valW, h, v.c_str(), true, EpdFontFamily::BOLD);
  hits.push_back({minusX - 4, y, minusX + b + 4, y + h, LoadDown, exercise, 0});
  hits.push_back({plusX - 4, y, plusX + b + 6, y + h, LoadUp, exercise, 0});
  return h;
}

void TrainingActivity::renderGymStep(int y) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = 16;
  const int width = w - 2 * pad;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  const int bigH = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  const int bottom = renderer.getScreenHeight() - m.buttonHintsHeight - GAP;
  Lift& l = gym;
  if (!l.day) return;
  int total = 0, all = 0;
  for (int i = 0; i < l.day->count; ++i) {
    total += l.done[i];
    all += l.day->ex[i].sets;
  }

  // "< Upper A          5 of 20 sets"
  const int headH = 56;
  renderer.drawIcon(TrainChevLIcon, pad - 4, y + (headH - ICON) / 2, ICON);
  renderer.drawText(NOTOSANS_18_FONT_ID, pad + 26, y + (headH - bigH) / 2, l.day->label, true, EpdFontFamily::BOLD);
  const std::string count = std::to_string(total) + " of " + std::to_string(all) + " sets";
  renderer.drawText(SMALL_FONT_ID, w - pad - renderer.getTextWidth(SMALL_FONT_ID, count.c_str(), EpdFontFamily::BOLD),
                    y + (headH - smallH) / 2, count.c_str(), true, EpdFontFamily::BOLD);
  hits.push_back({0, y, w / 2, y + headH, CloseGym, 0, 0});
  y += headH;

  // Step strip: 1 ... n, Finish.
  const int steps = l.day->count + 1;
  const int sgap = 6;
  const int sw = (width - (steps - 1) * sgap) / steps;
  const int sh = 56;
  for (int i = 0; i < steps; ++i) {
    const int sx = pad + i * (sw + sgap);
    const bool on = i == l.focus;
    const bool fin = i == l.day->count;
    const bool full = !fin && l.done[i] >= l.day->ex[i].sets;
    if (on) {
      renderer.fillRoundedRect(sx, y, sw, sh, 10, Color::Black);
    } else if (full) {
      renderer.fillRoundedRect(sx, y, sw, sh, 10, Color::LightGray);
      renderer.drawRoundedRect(sx, y, sw, sh, 2, 10, true);
    } else {
      renderer.drawRoundedRect(sx, y, sw, sh, 2, 10, true);
    }
    if (fin) {
      drawCentered(renderer, UI_10_FONT_ID, sx, y, sw, sh, "Save", !on, EpdFontFamily::BOLD);
    } else {
      const int nh = renderer.getLineHeight(UI_10_FONT_ID);
      const int top = y + (sh - nh - smallH) / 2;
      drawCentered(renderer, UI_10_FONT_ID, sx, top, sw, nh, std::to_string(i + 1).c_str(), !on, EpdFontFamily::BOLD);
      const std::string sfit =
          renderer.truncatedText(SMALL_FONT_ID, l.day->ex[i].shortName ? l.day->ex[i].shortName : "", sw - 2);
      drawCentered(renderer, SMALL_FONT_ID, sx, top + nh, sw, smallH, sfit.c_str(), !on);
    }
    hits.push_back({sx, y, sx + sw, y + sh, Step, i, 0});
  }
  y += sh + GAP;
  renderer.drawLine(0, y, w - 1, y, true);
  y += GAP;

  const int btnY = bottom - 60;
  if (l.focus >= l.day->count) {
    // Finish: what was done, then save.
    renderer.drawText(NOTOSANS_18_FONT_ID, pad, y, "Finish", true, EpdFontFamily::BOLD);
    y += bigH + 6;
    const int lineH = 36;
    for (int i = 0; i < l.day->count && y + lineH < btnY - 70; ++i) {
      const auto& e = l.day->ex[i];
      const int ty = y + (lineH - bodyH) / 2;
      renderer.drawText(UI_12_FONT_ID, pad, ty, e.name);
      const std::string v = std::to_string(l.done[i]) + "/" + std::to_string(e.sets) +
                            (e.unit[0] ? "   " + loadText(l.loads[i], e.unit) : "");
      renderer.drawText(UI_12_FONT_ID, w - pad - renderer.getTextWidth(UI_12_FONT_ID, v.c_str(), EpdFontFamily::BOLD),
                        ty, v.c_str(), true, EpdFontFamily::BOLD);
      y += lineH;
      renderer.drawLine(pad, y, w - pad, y, true);
    }
    if (!syncStatus.empty()) renderer.drawText(SMALL_FONT_ID, pad, btnY - smallH - 8, syncStatus.c_str());
    const int bw = (width - 10) * 2 / 3;
    drawButton(renderer, pad, btnY, bw, 60, "Save to Notion", true);
    hits.push_back({pad, btnY, pad + bw, btnY + 60, SaveLift, 0, 0});
    drawButton(renderer, pad + bw + 10, btnY, width - bw - 10, 60, "Clear", false);
    hits.push_back({pad + bw + 10, btnY, pad + width, btnY + 60, ClearLift, 0, 0});
    return;
  }

  const auto& e = l.day->ex[l.focus];
  for (const auto& line : renderer.wrappedText(NOTOSANS_18_FONT_ID, e.name, width, 2, EpdFontFamily::BOLD)) {
    renderer.drawText(NOTOSANS_18_FONT_ID, pad, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += bigH;
  }
  y += 6;
  int tx = pad + drawTag(renderer, pad, y, e.target, true) + 8;
  if (e.rest > 0) drawTag(renderer, tx, y, ("REST " + mmss(e.rest)).c_str(), false);
  y += smallH + 4 + GAP;
  if (e.cue) {
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, e.cue, width, 3)) {
      renderer.drawText(UI_10_FONT_ID, pad, y, line.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
    y += GAP;
  }
  y += renderSetRow(pad, y, width, l, l.focus, 64) + GAP;
  y += renderLoadRow(pad, y, width, l, l.focus, e.unit[0] == 's' ? "Hold" : e.unit[0] == 'c' ? "Height" : "Load") + GAP;

  // Rest: when the next set is due. E-ink does not tick, so it shows the time.
  const int restH = 64;
  if (y + restH < btnY - GAP) {
    renderer.drawRoundedRect(pad, y, width, restH, 2, RADIUS, true);
    const bool resting = restUntil > time(nullptr);
    renderer.drawText(SMALL_FONT_ID, pad + 14, y + 10, "REST", true, EpdFontFamily::BOLD);
    const std::string note = resting ? "Next set at " + clockText(restUntil) : "Starts when you tick a set";
    renderer.drawText(SMALL_FONT_ID, pad + 14, y + 12 + smallH, note.c_str());
    const std::string big = mmss(resting ? restSeconds : e.rest);
    renderer.drawText(NOTOSANS_18_FONT_ID,
                      w - pad - 14 - renderer.getTextWidth(NOTOSANS_18_FONT_ID, big.c_str(), EpdFontFamily::BOLD),
                      y + (restH - bigH) / 2, big.c_str(), true, EpdFontFamily::BOLD);
  }

  const int next = l.done[l.focus];
  const std::string label = next < e.sets                ? "Complete set " + std::to_string(next + 1)
                            : l.focus + 1 < l.day->count ? std::string("Next exercise")
                                                         : std::string("Finish");
  drawButton(renderer, pad, btnY, width, 60, label.c_str(), true);
  hits.push_back({pad, btnY, pad + width, btnY + 60, Primary, 0, 0});
}

void TrainingActivity::renderGrip(int y) {
  const int w = renderer.getScreenWidth();
  const int pad = 16;
  const int inset = 14;
  const int width = w - 2 * pad;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  Lift& l = grip;
  y += GAP;
  // Mon ... Fri, a dot when logged this week.
  const int chipH = 52;
  const int cgap = 8;
  const int cw = (width - 4 * cgap) / 5;
  const std::string mon = todayIso.empty() ? "" : mondayOf(todayIso);
  for (int i = 0; i < 5; ++i) {
    const auto* d = training::gripDay(i + 1);
    const int cx = pad + i * (cw + cgap);
    const bool sel = d == l.day;
    if (sel) {
      renderer.fillRoundedRect(cx, y, cw, chipH, RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(cx, y, cw, chipH, 2, RADIUS, true);
    }
    drawCentered(renderer, UI_12_FONT_ID, cx, y - 6, cw, chipH, DAYS[i + 1], !sel, EpdFontFamily::BOLD);
    bool logged = false;
    if (!mon.empty()) {
      const std::string iso = training::addDays(mon, i);
      for (const size_t s : sessionsOn(iso)) logged = logged || (sessions[s].type == "Grip" && sessions[s].done());
    }
    if (logged) renderer.fillRoundedRect(cx + cw / 2 - 4, y + chipH - 14, 8, 8, 4, sel ? Color::White : Color::Black);
    hits.push_back({cx, y, cx + cw, y + chipH, Chip, i, 0});
  }
  y += chipH + GAP;
  renderer.drawLine(0, y, w - 1, y, true);
  y += GAP;
  if (!l.day) return;

  int total = 0, all = 0;
  for (int i = 0; i < l.day->count; ++i) {
    const auto& e = l.day->ex[i];
    total += l.done[i];
    all += e.sets;
    const auto cue =
        e.cue ? renderer.wrappedText(SMALL_FONT_ID, e.cue, width - 2 * inset, 2) : std::vector<std::string>{};
    const int cardH = inset + bodyH + 8 + static_cast<int>(cue.size()) * smallH + 10 + 52 + 10 + 56 + inset;
    if (y + cardH > tabBarTop() - BTN_H - 2 * GAP) break;
    renderer.drawRoundedRect(pad, y, width, cardH, l.focus == i ? 3 : 2, 14, true);
    int cy = y + inset;
    renderer.drawText(UI_12_FONT_ID, pad + inset, cy, e.name, true, EpdFontFamily::BOLD);
    drawTag(renderer, pad + width - inset - tagWidth(renderer, e.target), cy, e.target, false);
    cy += bodyH + 8;
    for (const auto& c : cue) {
      renderer.drawText(SMALL_FONT_ID, pad + inset, cy, c.c_str());
      cy += smallH;
    }
    cy += 10;
    cy += renderSetRow(pad + inset, cy, width - 2 * inset, l, i, 52) + 10;
    const std::string lab = "Load, rest " + mmss(e.rest);
    renderLoadRow(pad + inset, cy, width - 2 * inset, l, i, lab.c_str());
    y += cardH + GAP;
  }
  const int by = tabBarTop() - GAP - BTN_H;
  const int bw = (width - 10) * 2 / 3;
  const std::string save = "Save to Notion, " + std::to_string(total) + " of " + std::to_string(all);
  drawButton(renderer, pad, by, bw, BTN_H, save.c_str(), true);
  hits.push_back({pad, by, pad + bw, by + BTN_H, SaveLift, 0, 0});
  drawButton(renderer, pad + bw + 10, by, width - bw - 10, BTN_H, "Clear", false);
  hits.push_back({pad + bw + 10, by, pad + width, by + BTN_H, ClearLift, 0, 0});
}

void TrainingActivity::renderHyrox(int y) {
  const int w = renderer.getScreenWidth();
  const int pad = 16;
  const int width = w - 2 * pad;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_10_FONT_ID);
  Lift& l = hyrox;
  y += GAP;
  // Program: 45 or 60 min.
  int n = 0;
  training::hyroxDays(n);
  const int pw = (width - 10) / 2;
  for (int i = 0; i < n && i < 2; ++i) {
    int c = 0;
    const auto* d = chipDay(Tab::Hyrox, i, c);
    const int px = pad + i * (pw + 10);
    const bool sel = d == l.day;
    const std::string label = std::string(d->label) + ", " + std::to_string(d->count) + " rounds";
    if (sel) {
      renderer.fillRoundedRect(px, y, pw, 48, 24, Color::Black);
    } else {
      renderer.drawRoundedRect(px, y, pw, 48, 2, 24, true);
    }
    drawCentered(renderer, UI_12_FONT_ID, px, y, pw, 48, label.c_str(), !sel, EpdFontFamily::BOLD);
    hits.push_back({px, y, px + pw, y + 48, Chip, i, 0});
  }
  y += 48 + GAP;
  if (!l.day) return;
  renderer.drawLine(0, y, w - 1, y, true);

  const int by = tabBarTop() - GAP - BTN_H;
  const int rowH = std::min(58, (by - GAP - y) / l.day->count);
  int firstOpen = -1;
  int total = 0;
  for (int i = 0; i < l.day->count; ++i) {
    if (l.done[i] == 0 && firstOpen < 0) firstOpen = i;
    total += l.done[i] > 0 ? 1 : 0;
  }
  for (int i = 0; i < l.day->count; ++i) {
    const auto& e = l.day->ex[i];
    const int ry = y + i * rowH;
    const bool done = l.done[i] > 0;
    const bool focus = i == l.focus && hxEdit;
    const int box = std::min(40, rowH - 12);
    const int bx = pad;
    const int byy = ry + (rowH - box) / 2;
    if (done) {
      drawCheck(renderer, bx, byy, box, true);
    } else {
      renderer.drawRoundedRect(bx, byy, box, box, i == firstOpen ? 3 : 1, 10, true);
      drawCentered(renderer, UI_12_FONT_ID, bx, byy, box, box, std::to_string(i + 1).c_str(), true,
                   EpdFontFamily::BOLD);
    }
    // Load on the right: tap to pick, then - and +.
    const int rightW = focus && e.unit[0] ? 150 : 70;
    const int rx = w - pad - rightW;
    if (e.unit[0] == '\0') {
      renderer.drawText(SMALL_FONT_ID, w - pad - renderer.getTextWidth(SMALL_FONT_ID, "Body", EpdFontFamily::BOLD),
                        ry + (rowH - smallH) / 2, "Body", true, EpdFontFamily::BOLD);
    } else if (focus) {
      const int b = 36;
      const int y0 = ry + (rowH - b) / 2;
      renderer.drawRoundedRect(rx, y0, b, b, 2, 8, true);
      drawCentered(renderer, UI_12_FONT_ID, rx, y0, b, b, "-", true, EpdFontFamily::BOLD);
      renderer.drawRoundedRect(w - pad - b, y0, b, b, 2, 8, true);
      drawCentered(renderer, UI_12_FONT_ID, w - pad - b, y0, b, b, "+", true, EpdFontFamily::BOLD);
      const std::string v = loadText(l.loads[i], e.unit);
      drawCentered(renderer, SMALL_FONT_ID, rx + b, ry, rightW - 2 * b, rowH, v.c_str(), true, EpdFontFamily::BOLD);
      hits.push_back({rx - 4, ry, rx + b + 4, ry + rowH, LoadDown, i, 0});
      hits.push_back({w - pad - b - 4, ry, w, ry + rowH, LoadUp, i, 0});
    } else {
      const std::string v = loadText(l.loads[i], e.unit);
      renderer.drawText(SMALL_FONT_ID, w - pad - renderer.getTextWidth(SMALL_FONT_ID, v.c_str(), EpdFontFamily::BOLD),
                        ry + (rowH - smallH) / 2, v.c_str(), true, EpdFontFamily::BOLD);
      hits.push_back({rx, ry, w, ry + rowH, HxFocus, i, 0});
    }
    const int tx = bx + box + 12;
    const int tw = rx - 8 - tx;
    const int top = ry + (rowH - bodyH - smallH) / 2;
    const std::string name =
        renderer.truncatedText(UI_10_FONT_ID, (std::string(e.name) + ", " + e.target).c_str(), tw, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, tx, top, name.c_str(), true, EpdFontFamily::BOLD);
    const std::string sub =
        renderer.truncatedText(SMALL_FONT_ID, ("Run 5 min, then. " + std::string(e.cue ? e.cue : "")).c_str(), tw);
    renderer.drawText(SMALL_FONT_ID, tx, top + bodyH, sub.c_str());
    hits.push_back({0, ry, rx, ry + rowH, HxToggle, i, 0});
    renderer.drawLine(0, ry + rowH, w - 1, ry + rowH, true);
  }
  const int bw = (width - 10) * 2 / 3;
  const std::string save = "Save to Notion, " + std::to_string(total) + " of " + std::to_string(l.day->count);
  drawButton(renderer, pad, by, bw, BTN_H, save.c_str(), true);
  hits.push_back({pad, by, pad + bw, by + BTN_H, SaveLift, 0, 0});
  drawButton(renderer, pad + bw + 10, by, width - bw - 10, BTN_H, "Clear", false);
  hits.push_back({pad + bw + 10, by, pad + width, by + BTN_H, ClearLift, 0, 0});
}

void TrainingActivity::renderTabBar() {
  const int w = renderer.getScreenWidth();
  const int top = tabBarTop();
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  renderer.drawLine(0, top, w - 1, top, 2, true);
  for (int i = 0; i < TAB_COUNT; ++i) {
    const int x0 = i * w / TAB_COUNT;
    const int x1 = (i + 1) * w / TAB_COUNT;
    const bool sel = i == static_cast<int>(tab);
    if (sel) renderer.fillRect(x0 + 12, top, x1 - x0 - 24, 4, true);
    const int iy = top + (TAB_BAR_HEIGHT - ICON - smallH - 2) / 2;
    renderer.drawIcon(TAB_ICONS[i], x0 + (x1 - x0 - ICON) / 2, iy, ICON);
    drawCentered(renderer, SMALL_FONT_ID, x0, iy + ICON + 2, x1 - x0, smallH, TAB_NAMES[i], true,
                 sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}
