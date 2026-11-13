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
#include "fontIds.h"

namespace {
constexpr char TICKS_PATH[] = "/.crosspoint/training/ticks.txt";
constexpr char LAST_SYNC_PATH[] = "/.crosspoint/training/last_sync.txt";
// Pull the plan again on open when the last sync is older than this.
constexpr time_t RESYNC_SECONDS = 60 * 60;
constexpr int GAP = 10;
constexpr int TAB_BAR_HEIGHT = 60;
constexpr int INSET = 14;
constexpr int STRIP_H = 64;
constexpr int CHIP_H = 44;
constexpr int ROW_MAX = 96;
constexpr int BOX = 38;
constexpr int BOX_GAP = 8;
constexpr int STEP_W = 44;
constexpr int LOAD_W = 84;
constexpr int SAVE_H = 52;
constexpr const char* TAB_NAMES[] = {"Today", "Gym", "Grip", "Hyrox"};
constexpr const char* DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* DAY_LETTER[] = {"S", "M", "T", "W", "T", "F", "S"};
constexpr const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

enum HitKind : int { StripDay = 1, Card, Chip, SetBox, LoadDown, LoadUp, SaveLift, GoTab, Row };

std::string dayTitle(const std::string& iso) {
  if (iso.size() < 10) return iso;
  const int wd = training::weekday(iso);
  const int month = std::atoi(iso.substr(5, 2).c_str());
  if (wd < 0 || month < 1 || month > 12) return iso;
  return std::string(DAYS[wd]) + " " + std::to_string(std::atoi(iso.substr(8, 2).c_str())) + " " + MONTHS[month - 1];
}

// "2026-11-13" -> "11/13", as the phone app names its logs.
std::string shortDay(const std::string& iso) {
  return iso.size() >= 10 ? iso.substr(5, 2) + "/" + iso.substr(8, 2) : iso;
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

void drawCentered(GfxRenderer& r, int font, int x, int y, int w, int h, const char* text, bool black,
                  EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int tw = r.getTextWidth(font, text, style);
  r.drawText(font, x + (w - tw) / 2, y + (h - r.getLineHeight(font)) / 2, text, black, style);
}
}  // namespace

TrainingActivity::TrainingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Training", renderer, mappedInput) {}

void TrainingActivity::onEnter() {
  Activity::onEnter();
  training::loadSessions(sessions);
  todayIso = training::today();
  dayIso = todayIso;

  // Default lift days: today's Notion gym row, else the weekly plan.
  int gymCount = 0;
  const training::LiftDay* gymDays = training::gymDays(gymCount);
  const int wd = todayIso.empty() ? -1 : training::weekday(todayIso);
  const training::LiftDay* gymDay = &gymDays[0];
  for (int i = 0; i < gymCount; ++i) {
    if (gymDays[i].dow == wd) gymDay = &gymDays[i];
  }
  for (const size_t i : sessionsOn(todayIso)) {
    for (int g = 0; g < gymCount; ++g) {
      if (sessions[i].type == gymDays[g].type) gymDay = &gymDays[g];
    }
  }
  openLiftDay(gym, gymDay);
  const training::LiftDay* gripToday = training::gripDay(wd);
  openLiftDay(grip, gripToday ? gripToday : training::gripDay(1));
  int hyroxCount = 0;
  openLiftDay(hyrox, training::hyroxDays(hyroxCount));
  loadTicks();

  if (!training::hasToken()) {
    syncStatus = "Add Notion key";
  } else {
    const int pending = training::pendingCount();
    std::string last;
    if (Storage.exists(LAST_SYNC_PATH)) Storage.readFileToString("TRAIN", LAST_SYNC_PATH, 32, last);
    const time_t lastSync = static_cast<time_t>(std::atoll(last.c_str()));
    const time_t now = time(nullptr);
    syncStatus = pending > 0 ? std::to_string(pending) + " to send" : "Tap to sync";
    syncPending = wifiSaved() && (pending > 0 || sessions.empty() || now - lastSync > RESYNC_SECONDS);
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

std::vector<size_t> TrainingActivity::sessionsOn(const std::string& iso) const {
  std::vector<size_t> out;
  for (size_t i = 0; i < sessions.size(); ++i) {
    // Hyrox is logged from its own tab and stays off the week view.
    if (sessions[i].date == iso && sessions[i].type != "Hyrox") out.push_back(i);
  }
  // Runs first, then the rest in Notion order.
  std::stable_sort(out.begin(), out.end(),
                   [&](size_t a, size_t b) { return sessions[a].isRun() && !sessions[b].isRun(); });
  return out;
}

void TrainingActivity::setTab(const Tab t) {
  {
    RenderLock lock(*this);
    tab = t;
  }
  requestUpdate();
}

void TrainingActivity::setDay(const std::string& iso) {
  {
    RenderLock lock(*this);
    dayIso = iso;
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
  l.first = 0;
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
      const size_t tab = line.find('\t', col);
      f.push_back(line.substr(col, tab == std::string::npos ? std::string::npos : tab - col));
      if (tab == std::string::npos) break;
      col = tab + 1;
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
  const float step = seconds ? 5.0f : (l.loads[exercise] >= 20 ? 2.5f : 1.0f);
  {
    RenderLock lock(*this);
    l.loads[exercise] = std::max(0.0f, l.loads[exercise] + dir * step);
    l.focus = exercise;
  }
  training::setLoad(e, l.loads[exercise]);
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
  if (isGym && std::string(l.day->key) == "legs") {
    for (auto& s : sessions) {
      if (s.type == "Strength" && s.week == week && !s.done() && !s.id.empty()) planned = &s;
    }
  }
  if (planned) {
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
    c.targetPace =
        isGym ? std::to_string(total) + "/" + std::to_string(all) + " sets" : std::to_string(total) + " sets";
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
    l.first = 0;
    restUntil = 0;
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
                             syncStatus = pending > 0 ? std::to_string(pending) + " to send" : "Tap to sync";
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
    } else {
      training::loadSessions(sessions);
    }
    syncStatus = message;
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
  if (mappedInput.wasReleased(B::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(B::Right)) return setTab(static_cast<Tab>((static_cast<int>(tab) + 1) % TAB_COUNT));
  if (mappedInput.wasReleased(B::Left)) {
    return setTab(static_cast<Tab>((static_cast<int>(tab) + TAB_COUNT - 1) % TAB_COUNT));
  }
  const bool down = mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::PageForward);
  const bool up = mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::PageBack);

  if (tab == Tab::Today) {
    if ((down || up) && !dayIso.empty()) return setDay(training::addDays(dayIso, down ? 1 : -1));
    if (mappedInput.wasReleased(B::Confirm)) {
      const auto on = sessionsOn(dayIso);
      for (const size_t i : on) {
        if (!sessions[i].done()) return openLog(i);
      }
      if (!on.empty()) return openLog(on.front());
    }
  } else {
    Lift& l = lift();
    const int count = l.day ? l.day->count : 0;
    if (down || up) {
      {
        RenderLock lock(*this);
        l.focus = (l.focus + (down ? 1 : count)) % (count + 1);
      }
      return requestUpdate();
    }
    if (mappedInput.wasReleased(B::Confirm)) {
      if (l.focus >= count) return saveLift(l);
      return tick(l, l.focus, -1);
    }
  }

  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return;
  if (y >= tabBarTop()) {
    const int t = x * TAB_COUNT / std::max(1, renderer.getScreenWidth());
    return setTab(static_cast<Tab>(std::min(t, TAB_COUNT - 1)));
  }
  if (y >= statusTop && y < statusBottom && x > renderer.getScreenWidth() / 2) return startSync();
  for (const auto& h : hits) {
    if (x < h.x0 || x >= h.x1 || y < h.y0 || y >= h.y1) continue;
    switch (h.kind) {
      case StripDay:
        return setDay(training::addDays(dayIso, h.a));
      case Card:
        return openLog(static_cast<size_t>(h.a));
      case GoTab:
        return setTab(static_cast<Tab>(h.a));
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
      case SetBox:
        return tick(lift(), h.a, h.b);
      case LoadDown:
        return changeLoad(lift(), h.a, -1);
      case LoadUp:
        return changeLoad(lift(), h.a, 1);
      case SaveLift:
        return saveLift(lift());
      case Row: {
        {
          RenderLock lock(*this);
          lift().focus = h.a;
        }
        return requestUpdate();
      }
      default:
        return;
    }
  }
}

// ---- render ----

int TrainingActivity::contentTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight + GAP;
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
  const int week = todayIso.empty() ? 0 : training::weekOf(todayIso);
  const std::string right = week > 0 ? "Week " + std::to_string(week) : "";
  GUI.drawHeader(renderer, Rect{0, m.topPadding, w, m.headerHeight}, "TRAINING.",
                 right.empty() ? nullptr : right.c_str());

  int y = contentTop();
  std::string title = TAB_NAMES[static_cast<int>(tab)];
  if (tab == Tab::Today && !dayIso.empty() && dayIso != todayIso) title = dayTitle(dayIso);
  renderStatusLine(y, title.c_str());
  y += renderer.getLineHeight(NOTOSANS_18_FONT_ID) + GAP;
  if (tab == Tab::Today) {
    renderToday(y);
  } else {
    renderLift(y);
  }
  renderTabBar();
  renderer.displayBuffer();
}

void TrainingActivity::renderStatusLine(const int y, const char* title) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;
  renderer.drawText(NOTOSANS_18_FONT_ID, pad, y, title, true, EpdFontFamily::BOLD);
  const std::string status = syncing ? "Syncing..." : syncStatus;
  const int titleW = renderer.getTextWidth(NOTOSANS_18_FONT_ID, title, EpdFontFamily::BOLD);
  const auto fit = renderer.wrappedText(SMALL_FONT_ID, status.c_str(), w - 2 * pad - titleW - 16, 1);
  if (!fit.empty()) {
    const int sw = renderer.getTextWidth(SMALL_FONT_ID, fit[0].c_str());
    const int sy = y + renderer.getLineHeight(NOTOSANS_18_FONT_ID) - renderer.getLineHeight(SMALL_FONT_ID) - 4;
    renderer.drawText(SMALL_FONT_ID, w - pad - sw, sy, fit[0].c_str());
  }
  statusTop = y;
  statusBottom = y + renderer.getLineHeight(NOTOSANS_18_FONT_ID);
}

void TrainingActivity::renderToday(int y) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;
  const int width = w - 2 * pad;
  const int bottom = tabBarTop() - GAP;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);

  if (dayIso.empty()) {
    renderer.drawText(UI_12_FONT_ID, pad, y, "Set the clock to see the plan.");
    return;
  }

  // Week strip, Monday first.
  const int wd = training::weekday(dayIso);
  const int fromMon = (wd + 6) % 7;
  const int cellW = width / 7;
  for (int i = 0; i < 7; ++i) {
    const std::string iso = training::addDays(dayIso, i - fromMon);
    const int x = pad + i * cellW;
    const bool sel = iso == dayIso;
    const bool isToday = iso == todayIso;
    if (sel) {
      renderer.fillRoundedRect(x + 2, y, cellW - 4, STRIP_H, 10, Color::Black);
    } else if (isToday) {
      renderer.drawRoundedRect(x + 2, y, cellW - 4, STRIP_H, 2, 10, true);
    }
    drawCentered(renderer, SMALL_FONT_ID, x, y + 4, cellW, smallH, DAY_LETTER[(i + 1) % 7], !sel);
    const std::string dnum = std::to_string(std::atoi(iso.substr(8, 2).c_str()));
    drawCentered(renderer, UI_12_FONT_ID, x, y + 4 + smallH, cellW, bodyH, dnum.c_str(), !sel, EpdFontFamily::BOLD);
    // Mark: filled = done, outline = planned, cross = missed.
    bool done = false, missed = false, planned = false;
    for (const size_t s : sessionsOn(iso)) {
      done |= sessions[s].done();
      missed |= sessions[s].status == "Missed";
      planned |= sessions[s].status == "Planned" || sessions[s].status.empty();
    }
    const int mx = x + cellW / 2 - 4;
    const int my = y + STRIP_H - 14;
    if (done) {
      renderer.fillRect(mx, my, 8, 8, !sel);
    } else if (missed) {
      renderer.drawLine(mx, my, mx + 7, my + 7, !sel);
      renderer.drawLine(mx + 7, my, mx, my + 7, !sel);
    } else if (planned) {
      renderer.drawRect(mx, my, 8, 8, !sel);
    }
    hits.push_back({x, y, x + cellW, y + STRIP_H, StripDay, i - fromMon, 0});
  }
  y += STRIP_H + 2 * GAP;

  const auto on = sessionsOn(dayIso);
  if (on.empty()) {
    renderer.drawText(NOTOSANS_16_FONT_ID, pad, y, "Rest day", true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(NOTOSANS_16_FONT_ID) + 4;
    renderer.drawText(SMALL_FONT_ID, pad, y, "Nothing planned in Notion.");
    y += smallH + 2 * GAP;
  }
  for (const size_t idx : on) {
    const auto& s = sessions[idx];
    // Measure first so the card fits its content.
    const auto titleLines =
        renderer.wrappedText(UI_12_FONT_ID, s.title.c_str(), width - 2 * INSET, 2, EpdFontFamily::BOLD);
    std::string hero;
    if (s.plannedKm > 0) {
      char buf[24];
      snprintf(buf, sizeof(buf), "%g km", s.plannedKm);
      hero = buf;
    }
    std::string result;
    if (s.done()) {
      char buf[96];
      if (s.isRun() && s.actualKm > 0) {
        snprintf(buf, sizeof(buf), "%s: %g km", s.status.c_str(), s.actualKm);
        result = buf;
        if (!s.actualPace.empty()) result += ", " + s.actualPace + "/km";
        if (s.hr > 0) result += ", HR " + std::to_string(s.hr);
      } else {
        result = s.status;
      }
      if (s.rpe > 0) result += ", RPE " + std::to_string(s.rpe);
    } else if (s.status == "Missed") {
      result = "Missed";
    }
    const int heroH = hero.empty() ? 0 : renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 4;
    const int fixed = INSET + smallH + 6 + static_cast<int>(titleLines.size()) * bodyH + heroH +
                      (result.empty() ? 0 : smallH + 8) + INSET;
    const int room = bottom - y - fixed - 6;
    const int maxWorkout = std::max(0, std::min(8, room / smallH));
    const auto workout = s.workout.empty() || maxWorkout == 0
                             ? std::vector<std::string>{}
                             : renderer.wrappedText(SMALL_FONT_ID, s.workout.c_str(), width - 2 * INSET, maxWorkout);
    const int cardH = fixed + static_cast<int>(workout.size()) * smallH + (workout.empty() ? 0 : 6);
    if (y + cardH > bottom) break;

    renderer.drawRoundedRect(pad, y, width, cardH, s.done() ? 1 : 2, 12, true);
    int cy = y + INSET;
    std::string meta = s.type;
    std::transform(meta.begin(), meta.end(), meta.begin(), ::toupper);
    if (!s.targetPace.empty()) meta += "  " + s.targetPace;
    const auto metaFit = renderer.wrappedText(SMALL_FONT_ID, meta.c_str(), width - 2 * INSET - 90, 1);
    if (!metaFit.empty())
      renderer.drawText(SMALL_FONT_ID, pad + INSET, cy, metaFit[0].c_str(), true, EpdFontFamily::BOLD);
    // Status chip on the right.
    const std::string chip = s.id.empty() ? "Sending" : s.status.empty() ? "Planned" : s.status;
    const int chipW = renderer.getTextWidth(SMALL_FONT_ID, chip.c_str()) + 16;
    const int chipX = pad + width - INSET - chipW;
    if (s.done()) {
      renderer.fillRoundedRect(chipX, cy - 2, chipW, smallH + 4, 6, Color::Black);
    } else {
      renderer.drawRoundedRect(chipX, cy - 2, chipW, smallH + 4, 1, 6, true);
    }
    renderer.drawText(SMALL_FONT_ID, chipX + 8, cy, chip.c_str(), !s.done());
    cy += smallH + 6;
    for (const auto& l : titleLines) {
      renderer.drawText(UI_12_FONT_ID, pad + INSET, cy, l.c_str(), true, EpdFontFamily::BOLD);
      cy += bodyH;
    }
    if (!hero.empty()) {
      renderer.drawText(NOTOSANS_18_FONT_ID, pad + INSET, cy + 2, hero.c_str(), true, EpdFontFamily::BOLD);
      cy += heroH;
    }
    if (!workout.empty()) {
      cy += 6;
      for (const auto& l : workout) {
        renderer.drawText(SMALL_FONT_ID, pad + INSET, cy, l.c_str());
        cy += smallH;
      }
    }
    if (!result.empty()) {
      cy += 8;
      renderer.fillRect(pad + INSET, cy, 3, smallH, true);
      renderer.drawText(SMALL_FONT_ID, pad + INSET + 10, cy, result.c_str(), true, EpdFontFamily::BOLD);
    }
    hits.push_back({pad, y, pad + width, y + cardH, Card, static_cast<int>(idx), 0});
    y += cardH + GAP;
  }

  // Gym and grip planned for this weekday, one tap away.
  int gymCount = 0;
  const auto* gymDays = training::gymDays(gymCount);
  std::string gymLabel;
  for (int i = 0; i < gymCount; ++i) {
    if (gymDays[i].dow == wd) gymLabel = gymDays[i].label;
  }
  const auto* gripToday = training::gripDay(wd);
  const struct {
    const char* label;
    std::string value;
    Tab target;
  } links[] = {{"Gym", gymLabel, Tab::Gym}, {"Grip", gripToday ? gripToday->ex[0].name : "", Tab::Grip}};
  for (const auto& l : links) {
    if (l.value.empty() || y + bodyH + 2 * GAP > bottom) continue;
    const std::string text = std::string(l.label) + ": " + l.value + "  >";
    renderer.drawText(UI_12_FONT_ID, pad, y + GAP / 2, text.c_str());
    hits.push_back({pad, y, pad + width, y + bodyH + GAP, GoTab, static_cast<int>(l.target), 0});
    y += bodyH + GAP;
  }
}

void TrainingActivity::renderLift(int y) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;
  const int width = w - 2 * pad;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  Lift& l = lift();

  // Day chips.
  int chipCount = 1;
  chipDay(tab, 0, chipCount);
  const int chipW = (width - (chipCount - 1) * 8) / chipCount;
  for (int i = 0; i < chipCount; ++i) {
    int n = 0;
    const training::LiftDay* d = chipDay(tab, i, n);
    if (!d) continue;
    const int x = pad + i * (chipW + 8);
    const bool sel = d == l.day;
    if (sel) {
      renderer.fillRoundedRect(x, y, chipW, CHIP_H, 10, Color::Black);
    } else {
      renderer.drawRoundedRect(x, y, chipW, CHIP_H, 1, 10, true);
    }
    drawCentered(renderer, UI_12_FONT_ID, x, y, chipW, CHIP_H, d->label, !sel, EpdFontFamily::BOLD);
    hits.push_back({x, y, x + chipW, y + CHIP_H, Chip, i, 0});
  }
  y += CHIP_H + GAP;
  if (!l.day) return;

  // Rest or progress line.
  int total = 0;
  int all = 0;
  for (int i = 0; i < l.day->count; ++i) {
    total += l.done[i];
    all += l.day->ex[i].sets;
  }
  const bool rounds = &l == &hyrox;
  std::string info = std::to_string(total) + " of " + std::to_string(all) + (rounds ? " rounds" : " sets");
  if (l.day->note) info = std::string(l.day->note) + " " + info + " done.";
  if (restUntil > time(nullptr)) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Rest %d:%02d, next set at %s", restSeconds / 60, restSeconds % 60,
             clockText(restUntil).c_str());
    info = buf;
  }
  renderer.drawText(SMALL_FONT_ID, pad, y, info.c_str(), true, EpdFontFamily::BOLD);
  y += smallH + GAP;

  // Exercise rows, scrolled so the focused one is visible.
  const int saveTop = tabBarTop() - GAP - SAVE_H;
  const int avail = saveTop - GAP - y - smallH;
  const int rowMin = 6 + bodyH + 4 + BOX + 8;  // name, controls, gap
  const int visible = std::max(1, std::min(l.day->count, avail / rowMin));
  const int rowH = std::max(rowMin, std::min(ROW_MAX, avail / visible));
  if (l.focus < l.day->count) {
    if (l.focus < l.first) l.first = l.focus;
    if (l.focus >= l.first + visible) l.first = l.focus - visible + 1;
  }
  l.first = std::max(0, std::min(l.first, l.day->count - visible));
  const int last = std::min(l.day->count, l.first + visible);
  renderer.drawLine(pad, y, w - pad, y);
  for (int i = l.first; i < last; ++i) {
    const auto& e = l.day->ex[i];
    if (l.focus == i) renderer.fillRect(pad - 10, y + 10, 4, rowH - 20, true);
    const int targetW = renderer.getTextWidth(SMALL_FONT_ID, e.target);
    const auto name = renderer.wrappedText(UI_12_FONT_ID, e.name, width - targetW - 12, 1, EpdFontFamily::BOLD);
    if (!name.empty()) renderer.drawText(UI_12_FONT_ID, pad, y + 6, name[0].c_str(), true, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, w - pad - targetW, y + 6 + bodyH - smallH - 2, e.target);
    hits.push_back({0, y, w, y + 8 + bodyH, Row, i, 0});

    // Load: [-] 50 kg [+], or Bodyweight.
    const int cy = y + 6 + bodyH + 4;
    if (e.unit[0] == '\0') {
      renderer.drawText(UI_12_FONT_ID, pad, cy + (BOX - bodyH) / 2, "Bodyweight");
    } else {
      renderer.drawRoundedRect(pad, cy, STEP_W, BOX, 1, 8, true);
      drawCentered(renderer, UI_12_FONT_ID, pad, cy, STEP_W, BOX, "-", true, EpdFontFamily::BOLD);
      hits.push_back({pad, cy, pad + STEP_W, cy + BOX, LoadDown, i, 0});
      const std::string load = loadText(l.loads[i], e.unit);
      drawCentered(renderer, UI_12_FONT_ID, pad + STEP_W, cy, LOAD_W, BOX, load.c_str(), true, EpdFontFamily::BOLD);
      const int px = pad + STEP_W + LOAD_W;
      renderer.drawRoundedRect(px, cy, STEP_W, BOX, 1, 8, true);
      drawCentered(renderer, UI_12_FONT_ID, px, cy, STEP_W, BOX, "+", true, EpdFontFamily::BOLD);
      hits.push_back({px, cy, px + STEP_W, cy + BOX, LoadUp, i, 0});
    }

    // Set boxes, right aligned; filled = done. A Hyrox station has one box,
    // numbered with its round.
    for (int s = 0; s < e.sets; ++s) {
      const std::string num = std::to_string(rounds ? i + 1 : s + 1);
      const int bx = w - pad - (e.sets - s) * BOX - (e.sets - s - 1) * BOX_GAP;
      if (s < l.done[i]) {
        renderer.fillRoundedRect(bx, cy, BOX, BOX, 8, Color::Black);
        drawCentered(renderer, UI_10_FONT_ID, bx, cy, BOX, BOX, num.c_str(), false, EpdFontFamily::BOLD);
      } else {
        renderer.drawRoundedRect(bx, cy, BOX, BOX, 2, 8, true);
        drawCentered(renderer, UI_10_FONT_ID, bx, cy, BOX, BOX, num.c_str(), true);
      }
      hits.push_back({bx - BOX_GAP / 2, cy - 6, bx + BOX + BOX_GAP / 2, cy + BOX + 6, SetBox, i, s});
    }
    y += rowH;
    renderer.drawLine(pad, y, w - pad, y);
  }
  if (l.first > 0 || last < l.day->count) {
    const std::string more = std::to_string(l.first + 1) + "-" + std::to_string(last) + " of " +
                             std::to_string(l.day->count) + "  (Up/Down for more)";
    renderer.drawText(SMALL_FONT_ID, pad, y + 4, more.c_str());
  }

  // Save.
  const bool sel = l.focus >= l.day->count;
  if (sel) {
    renderer.fillRoundedRect(pad, saveTop, width, SAVE_H, 10, Color::Black);
  } else {
    renderer.drawRoundedRect(pad, saveTop, width, SAVE_H, 2, 10, true);
  }
  const std::string save = "Save to Notion (" + std::to_string(total) + "/" + std::to_string(all) + ")";
  drawCentered(renderer, UI_12_FONT_ID, pad, saveTop, width, SAVE_H, save.c_str(), !sel, EpdFontFamily::BOLD);
  hits.push_back({pad, saveTop, pad + width, saveTop + SAVE_H, SaveLift, 0, 0});
}

void TrainingActivity::renderTabBar() {
  const int w = renderer.getScreenWidth();
  const int top = tabBarTop();
  renderer.drawLine(0, top, w - 1, top);
  for (int i = 0; i < TAB_COUNT; ++i) {
    const int x0 = i * w / TAB_COUNT;
    const int x1 = (i + 1) * w / TAB_COUNT;
    const bool sel = i == static_cast<int>(tab);
    if (sel) renderer.fillRect(x0 + (x1 - x0) / 4, top, (x1 - x0) / 2, 4, true);
    const auto style = sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, TAB_NAMES[i], style);
    renderer.drawText(UI_12_FONT_ID, x0 + (x1 - x0 - tw) / 2,
                      top + (TAB_BAR_HEIGHT - renderer.getLineHeight(UI_12_FONT_ID)) / 2, TAB_NAMES[i], true, style);
  }
}
