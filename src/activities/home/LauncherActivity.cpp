#include "LauncherActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <ctime>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/study/CompoundActivity.h"
#include "activities/study/StudySessionActivity.h"
#include "activities/study/TrainingActivity.h"
#include "components/UITheme.h"
#include "components/icons/blocks.h"
#include "components/icons/bookmark.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/pixelAppIcons.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/trainIcons.h"
#include "components/icons/transfer.h"
#include "fontIds.h"
#include "study/StudyDeck.h"
#include "study/Training.h"
#include "util/BookProgress.h"

namespace {
constexpr int PAD = 16;
constexpr int GAP = 12;
constexpr int ICON = 32;  // drawer icons
constexpr int SMALL_ICON = 24;
constexpr int TILE_ICON = 64;  // 8-bit app icons
constexpr int EDGE = 40;       // right-edge band that starts the drawer swipe
constexpr int DRAWER_ROW = 68;
constexpr int ROW_H = 52;  // Today card lines
constexpr const char* WEEKDAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct Tile {
  const char* label;
  const char* hint;
  const uint8_t* icon;
};
// Indexed by App - 1 (the Continue card is drawn separately).
constexpr Tile TILES[] = {
    {"Compound", "Daily brief", PixelCompoundIcon},
    {"IELTS", "Vocab", PixelVocabIcon},
    {"Chinese", "Hanzi", PixelChineseIcon},
    {"Training", "Plan", PixelTrainIcon},
};
constexpr Tile TOOLS[] = {
    {"Browse files", nullptr, FolderIcon},
    {"Library", "Your books", LibraryIcon},
    {"File transfer", "USB or WiFi", TransferIcon},
    {"Settings", nullptr, Settings2Icon},
};

std::string initials(const std::string& title) {
  std::string out;
  bool start = true;
  for (const char c : title) {
    if (c == ' ') {
      start = true;
    } else if (start && isalnum(static_cast<unsigned char>(c)) && out.size() < 2) {
      out += static_cast<char>(toupper(static_cast<unsigned char>(c)));
      start = false;
    } else {
      start = false;
    }
  }
  return out.empty() ? "B" : out;
}

void drawCentered(const GfxRenderer& r, int font, int x, int y, int w, int h, const char* text, bool black,
                  EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int tw = r.getTextWidth(font, text, style);
  r.drawText(font, x + (w - tw) / 2, y + (h - r.getLineHeight(font)) / 2, text, black, style);
}

std::string studyText(const StudyDeck::Summary& s) {
  if (s.reviews == 0 && s.fresh == 0) return "All done for today";
  std::string t = std::to_string(s.reviews) + " review" + (s.reviews == 1 ? "" : "s");
  if (s.fresh > 0) t += ", " + std::to_string(s.fresh) + " new";
  return t;
}

std::string minutes(const int cards) { return "~" + std::to_string(std::max(1, (cards * 25 + 59) / 60)) + " min"; }
}  // namespace

LauncherActivity::LauncherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Home", renderer, mappedInput) {}

void LauncherActivity::onEnter() {
  Activity::onEnter();
  const auto& books = RECENT_BOOKS.getBooks();
  for (const auto& b : books) {
    if (RecentBooksStore::isMissing(b)) continue;
    bookPath = b.path;
    bookTitle = b.title.empty() ? b.path.substr(b.path.find_last_of('/') + 1) : b.title;
    bookAuthor = b.author;
    break;
  }
  selected = bookPath.empty() ? 1 : 0;
  bookPercent = bookPath.empty() ? -1 : loadBookProgress(bookPath);
  buildToday();
  requestUpdate();
}

void LauncherActivity::buildToday() {
  today.clear();
  const std::string iso = training::today();
  // Training: the day's run, gym and grip, like the Training calendar.
  if (!iso.empty()) {
    std::vector<training::Session> sessions;
    training::loadSessions(sessions);
    const int wd = training::weekday(iso);
    bool gymDone = false;
    bool gripDone = false;
    for (const auto& ss : sessions) {
      if (ss.date != iso) continue;
      if (ss.isRun()) {
        std::string t = ss.title;
        const size_t colon = t.find(": ");
        if (!t.empty() && t[0] == 'W' && colon != std::string::npos && colon < 12) t = t.substr(colon + 2);
        today.push_back(
            {App::Training, TrainRunIcon, "RUN", t, ss.targetPace.empty() ? "" : ss.targetPace + " /km", ss.done()});
      } else if (ss.type == "Grip") {
        gripDone = gripDone || ss.done();
      } else if (ss.type == "Upper A" || ss.type == "Upper B" || ss.type == "Arms" || ss.type == "Strength") {
        gymDone = gymDone || ss.done();
      }
    }
    int n = 0;
    const auto* days = training::gymDays(n);
    for (int i = 0; i < n; ++i) {
      if (days[i].dow != wd) continue;
      int sets = 0;
      for (int k = 0; k < days[i].count; ++k) sets += days[i].ex[k].sets;
      today.push_back({App::Training, TrainGymIcon, "GYM", days[i].label, std::to_string(sets) + " sets", gymDone});
    }
    if (const auto* g = training::gripDay(wd)) {
      std::string names;
      int sets = 0;
      for (int k = 0; k < g->count; ++k) {
        names += (k ? ", " : "") + std::string(g->ex[k].name);
        sets += g->ex[k].sets;
      }
      today.push_back({App::Training, TrainGripIcon, "GRIP", names, std::to_string(sets) + " sets", gripDone});
    }
  }
  // Study decks.
  StudyDeck::Summary sum;
  if (StudyDeck::summary("vocab", sum)) {
    const int n = sum.reviews + sum.fresh;
    today.push_back({App::Vocab, TrainBookIcon, "IELTS", studyText(sum), n ? minutes(n) : "", n == 0});
  }
  if (StudyDeck::summary("chinese", sum)) {
    const int n = sum.reviews + sum.fresh;
    today.push_back({App::Chinese, TrainHanziIcon, "CHINESE", studyText(sum), n ? minutes(n) : "", n == 0});
  }
  // Compound: today's markets brief, read or not.
  std::string index;
  if (!iso.empty() && Storage.exists("/.crosspoint/compound/index.txt") &&
      Storage.readFileToString("HOME", "/.crosspoint/compound/index.txt", 8192, index)) {
    size_t start = 0;
    while (start < index.size()) {
      size_t end = index.find('\n', start);
      if (end == std::string::npos) end = index.size();
      const std::string line = index.substr(start, end - start);
      start = end + 1;
      // file \t type \t date \t title
      const size_t a = line.find('\t');
      const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
      const size_t c = b == std::string::npos ? b : line.find('\t', b + 1);
      const size_t d = c == std::string::npos ? c : line.find('\t', c + 1);
      if (d == std::string::npos || line.compare(a + 1, b - a - 1, "news") != 0 ||
          line.compare(b + 1, c - b - 1, iso) != 0) {
        continue;
      }
      const std::string file = line.substr(0, a);
      const std::string cb = file.substr(0, file.find_last_of('.')) + ".cb";
      std::string read;
      if (Storage.exists("/.crosspoint/compound/read.txt")) {
        Storage.readFileToString("HOME", "/.crosspoint/compound/read.txt", 2048, read);
      }
      const bool done = read.find(cb + "\n") != std::string::npos;
      today.push_back(
          {App::Compound, TrainChartIcon, "COMPOUND", line.substr(c + 1, d - c - 1), done ? "Read" : "5 min", done});
      break;
    }
  }
}

int LauncherActivity::drawerLeft() const { return renderer.getScreenWidth() / 4; }

int LauncherActivity::tileAt(const int x, const int y) const {
  if (cardBox.has(x, y)) return 0;
  for (int i = 0; i < 4; ++i) {
    if (tileBoxes[i].has(x, y)) return i + 1;
  }
  return -1;
}

int LauncherActivity::toolAt(const int x, const int y) const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int top = m.topPadding + m.headerHeight + 64;
  if (x < drawerLeft() || y < top) return -1;
  const int row = (y - top) / DRAWER_ROW;
  return row < TOOL_COUNT ? row : -1;
}

void LauncherActivity::setDrawer(const bool open) {
  {
    RenderLock lock;
    drawerOpen = open;
    drawerSelected = 0;
  }
  requestUpdate();
}

void LauncherActivity::openApp(const App app) {
  std::unique_ptr<Activity> screen;
  switch (app) {
    case App::Continue:
      if (!bookPath.empty()) activityManager.goToReader(bookPath);
      return;
    case App::Training:
      screen = makeUniqueNoThrow<TrainingActivity>(renderer, mappedInput);
      break;
    case App::Compound:
      screen = makeUniqueNoThrow<CompoundActivity>(renderer, mappedInput);
      break;
    case App::Vocab:
      screen = makeUniqueNoThrow<StudySessionActivity>(renderer, mappedInput, "vocab", "IELTS / TOEFL");
      break;
    case App::Chinese:
      screen = makeUniqueNoThrow<StudySessionActivity>(renderer, mappedInput, "chinese", "Chinese");
      break;
  }
  if (!screen) {
    LOG_ERR("HOME", "OOM: app screen");
    return;
  }
  startActivityForResult(std::move(screen), [this](const ActivityResult&) { requestUpdate(); });
}

void LauncherActivity::openTool(const Tool tool) {
  drawerOpen = false;
  switch (tool) {
    case Tool::Files:
      activityManager.goToFileBrowser();
      break;
    case Tool::Library:
      activityManager.goToLibrary();
      break;
    case Tool::Transfer:
      activityManager.goToFileTransfer();
      break;
    case Tool::Settings:
      activityManager.goToSettings();
      break;
  }
}

void LauncherActivity::loop() {
  using B = MappedInputManager::Button;
  const int w = renderer.getScreenWidth();

  int sx = 0;
  int sy = 0;
  const auto swipe = mappedInput.wasSwipe(&sx, &sy);
  if (!drawerOpen && swipe == MappedInputManager::SwipeDir::Left && sx >= w - EDGE) return setDrawer(true);
  if (drawerOpen && swipe == MappedInputManager::SwipeDir::Right) return setDrawer(false);

  if (mappedInput.wasReleased(B::Back)) return setDrawer(!drawerOpen);

  if (drawerOpen) {
    if (mappedInput.wasReleased(B::Down) || mappedInput.wasReleased(B::Right)) {
      {
        RenderLock lock;
        drawerSelected = (drawerSelected + 1) % TOOL_COUNT;
      }
      return requestUpdate();
    }
    if (mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::Left)) {
      {
        RenderLock lock;
        drawerSelected = (drawerSelected + TOOL_COUNT - 1) % TOOL_COUNT;
      }
      return requestUpdate();
    }
    if (mappedInput.wasReleased(B::Confirm)) return openTool(static_cast<Tool>(drawerSelected));
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      if (x < drawerLeft()) return setDrawer(false);
      if (sleepBox.has(x, y)) {
        drawerOpen = false;
        activityManager.requestSleep();
        return;
      }
      const int t = toolAt(x, y);
      if (t >= 0) openTool(static_cast<Tool>(t));
    }
    return;
  }

  const int first = bookPath.empty() ? 1 : 0;
  int next = selected;
  if (mappedInput.wasReleased(B::Right)) next = selected + 1;
  if (mappedInput.wasReleased(B::Left)) next = selected - 1;
  if (mappedInput.wasReleased(B::Down)) next = selected + 1;
  if (mappedInput.wasReleased(B::Up)) next = selected - 1;
  if (next != selected && next >= first && next < APP_COUNT) {
    {
      RenderLock lock;
      selected = next;
    }
    return requestUpdate();
  }
  if (mappedInput.wasReleased(B::Confirm)) return openApp(static_cast<App>(selected));

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    if (menuBox.has(x, y)) return setDrawer(true);
    for (size_t i = 0; i < rowBoxes.size() && i < today.size(); ++i) {
      if (rowBoxes[i].has(x, y)) return openApp(today[i].app);
    }
    const int t = tileAt(x, y);
    if (t > 0 || (t == 0 && !bookPath.empty())) openApp(static_cast<App>(t));
  }
}

void LauncherActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int width = w - 2 * PAD;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  const int bigH = renderer.getLineHeight(NOTOSANS_18_FONT_ID);

  // Date as the header title; battery sits on the right as usual.
  std::string date;
  std::string clock;
  const time_t now = time(nullptr);
  if (now > 1577836800) {
    struct tm t{};
    localtime_r(&now, &t);
    date = std::string(WEEKDAYS[t.tm_wday]) + " " + std::to_string(t.tm_mday) + " " + MONTHS[t.tm_mon];
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    clock = buf;
  }
  GUI.drawHeader(renderer, Rect{0, m.topPadding, w, m.headerHeight}, date.empty() ? "Home" : date.c_str(), nullptr,
                 false);

  // Clock and the menu button.
  int y = m.topPadding + m.headerHeight + 4;
  const int clockRow = 56;
  if (!clock.empty()) {
    renderer.drawText(NOTOSANS_18_FONT_ID, PAD, y + (clockRow - bigH) / 2, clock.c_str(), true, EpdFontFamily::BOLD);
  }
  const int mb = 48;
  menuBox = {w - PAD - mb, y + (clockRow - mb) / 2, w - PAD + 4, y + (clockRow + mb) / 2};
  renderer.drawRoundedRect(menuBox.x0, menuBox.y0, mb, mb, 2, 12, true);
  renderer.drawIcon(TrainMenuIcon, menuBox.x0 + (mb - SMALL_ICON) / 2, menuBox.y0 + (mb - SMALL_ICON) / 2, SMALL_ICON);
  menuBox.x0 -= 8;
  y += clockRow + 6;

  // Today: what each app wants from you today.
  rowBoxes.clear();
  if (!today.empty()) {
    int doneCount = 0;
    for (const auto& r : today) doneCount += r.done ? 1 : 0;
    const int headH = bodyH + 16;
    const int cardH = headH + static_cast<int>(today.size()) * ROW_H + 6;
    renderer.drawRoundedRect(PAD, y, width, cardH, 2, 16, true);
    renderer.drawText(UI_12_FONT_ID, PAD + 16, y + 10, "Today", true, EpdFontFamily::BOLD);
    const std::string done = std::to_string(doneCount) + " of " + std::to_string(today.size()) + " done";
    renderer.drawText(SMALL_FONT_ID,
                      w - PAD - 16 - renderer.getTextWidth(SMALL_FONT_ID, done.c_str(), EpdFontFamily::BOLD),
                      y + 10 + bodyH - smallH - 2, done.c_str(), true, EpdFontFamily::BOLD);
    int ry = y + headH;
    for (const auto& r : today) {
      renderer.drawLine(PAD + 12, ry, w - PAD - 12, ry, true);
      const int box = 36;
      const int bx = PAD + 14;
      const int by = ry + (ROW_H - box) / 2;
      if (r.done) {
        renderer.fillRoundedRect(bx, by, box, box, 10, Color::Black);
        const int c = bx + box / 2, mid = by + box / 2;
        renderer.drawLine(c - 8, mid, c - 2, mid + 6, 3, false);
        renderer.drawLine(c - 2, mid + 6, c + 9, mid - 7, 3, false);
      } else {
        renderer.drawRoundedRect(bx, by, box, box, 2, 10, true);
        renderer.drawIcon(r.icon, bx + (box - SMALL_ICON) / 2, by + (box - SMALL_ICON) / 2, SMALL_ICON);
      }
      const int metaW = r.meta.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, r.meta.c_str(), EpdFontFamily::BOLD);
      const int tx = bx + box + 12;
      const int right = w - PAD - 16 - (r.done ? 0 : SMALL_ICON + 4);
      const int tw = right - tx - (metaW ? metaW + 10 : 0);
      const int top = ry + (ROW_H - smallH - bodyH) / 2;
      renderer.drawText(SMALL_FONT_ID, tx, top, r.label.c_str(), true, EpdFontFamily::BOLD);
      const auto style = r.done ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
      const std::string fit = renderer.truncatedText(UI_12_FONT_ID, r.text.c_str(), tw, style);
      renderer.drawText(UI_12_FONT_ID, tx, top + smallH, fit.c_str(), true, style);
      if (r.done) {
        const int lw = renderer.getTextWidth(UI_12_FONT_ID, fit.c_str(), style);
        renderer.drawLine(tx, top + smallH + bodyH / 2 + 1, tx + lw, top + smallH + bodyH / 2 + 1, 2, true);
      }
      if (metaW) {
        renderer.drawText(SMALL_FONT_ID, right - metaW, ry + (ROW_H - smallH) / 2, r.meta.c_str(), true,
                          EpdFontFamily::BOLD);
      }
      if (!r.done) {
        renderer.drawIcon(TrainChevRIcon, w - PAD - 12 - SMALL_ICON, ry + (ROW_H - SMALL_ICON) / 2, SMALL_ICON);
      }
      rowBoxes.push_back({PAD, ry, w - PAD, ry + ROW_H});
      ry += ROW_H;
    }
    y += cardH + GAP;
  }

  // Continue reading.
  const int coverW = 52;
  const int coverH = 72;
  const int contH = coverH + 24;
  const bool cardSel = selected == 0;
  renderer.drawRoundedRect(PAD, y, width, contH, cardSel ? 3 : 2, 16, true);
  cardBox = {PAD, y, w - PAD, y + contH};
  const int cx = PAD + 14;
  const int cy = y + (contH - coverH) / 2;
  if (bookPath.empty()) {
    renderer.drawRoundedRect(cx, cy, coverW, coverH, 2, 6, true);
    renderer.drawIcon(TrainBookIcon, cx + (coverW - SMALL_ICON) / 2, cy + (coverH - SMALL_ICON) / 2, SMALL_ICON);
  } else {
    renderer.fillRoundedRect(cx, cy, coverW, coverH, 6, Color::Black);
    drawCentered(renderer, UI_12_FONT_ID, cx, cy, coverW, coverH, initials(bookTitle).c_str(), false,
                 EpdFontFamily::BOLD);
  }
  const int tx = cx + coverW + 14;
  const int tw = w - PAD - 14 - tx;
  int ty = y + (contH - smallH - bodyH - smallH - (bookPercent >= 0 ? 14 : 4)) / 2;
  renderer.drawText(SMALL_FONT_ID, tx, ty, "CONTINUE READING", true, EpdFontFamily::BOLD);
  ty += smallH + 2;
  const std::string title = bookPath.empty() ? "No book open yet" : bookTitle;
  renderer.drawText(UI_12_FONT_ID, tx, ty,
                    renderer.truncatedText(UI_12_FONT_ID, title.c_str(), tw, EpdFontFamily::BOLD).c_str(), true,
                    EpdFontFamily::BOLD);
  ty += bodyH + 2;
  std::string sub = bookPath.empty() ? "Open one from Library" : bookAuthor;
  if (bookPercent >= 0) sub += (sub.empty() ? "" : "  ·  ") + std::to_string(bookPercent) + "%";
  renderer.drawText(SMALL_FONT_ID, tx, ty, renderer.truncatedText(SMALL_FONT_ID, sub.c_str(), tw).c_str());
  if (bookPercent >= 0) {
    // Progress bar under the author.
    const int by = ty + smallH + 4;
    renderer.fillRoundedRect(tx, by, tw, 6, 3, Color::LightGray);
    renderer.fillRoundedRect(tx, by, std::max(6, tw * bookPercent / 100), 6, 3, Color::Black);
  }
  y += contH + GAP;

  // Apps: one row of four.
  const int tgap = 10;
  const int tileW = (width - 3 * tgap) / 4;
  const int tileH = std::max(110, std::min(150, h - m.buttonHintsHeight - GAP - y));
  for (int i = 0; i < 4; ++i) {
    const int x = PAD + i * (tileW + tgap);
    const bool sel = selected == i + 1;
    renderer.drawRoundedRect(x, y, tileW, tileH, sel ? 3 : 2, 14, true);
    const int blockH = TILE_ICON + 6 + bodyH + smallH;
    int iy = y + (tileH - blockH) / 2;
    renderer.drawIcon(TILES[i].icon, x + (tileW - TILE_ICON) / 2, iy, TILE_ICON);
    iy += TILE_ICON + 6;
    drawCentered(renderer, SMALL_FONT_ID, x, iy, tileW, bodyH, TILES[i].label, true, EpdFontFamily::BOLD);
    drawCentered(renderer, SMALL_FONT_ID, x, iy + bodyH, tileW, smallH, TILES[i].hint, true);
    tileBoxes[i] = {x, y, x + tileW, y + tileH};
  }

  // Device drawer.
  if (drawerOpen) {
    const int left = drawerLeft();
    const int dTop = m.topPadding + m.headerHeight;
    renderer.fillRectDither(0, dTop, left, h - dTop, Color::LightGray);
    renderer.fillRect(left, dTop, w - left, h - dTop, false);
    renderer.fillRect(left, dTop, 3, h - dTop, true);
    renderer.drawText(NOTOSANS_18_FONT_ID, left + 18, dTop + (60 - bigH) / 2, "Menu", true, EpdFontFamily::BOLD);
    renderer.drawIcon(TrainChevRIcon, w - 14 - SMALL_ICON, dTop + (60 - SMALL_ICON) / 2, SMALL_ICON);
    renderer.drawLine(left, dTop + 60, w - 1, dTop + 60, 2, true);
    int dy = dTop + 64;
    for (int i = 0; i < TOOL_COUNT; ++i) {
      const bool sel = i == drawerSelected;
      if (sel) renderer.drawRoundedRect(left + 8, dy + 4, w - left - 14, DRAWER_ROW - 8, 3, 10, true);
      renderer.drawIcon(TOOLS[i].icon, left + 18, dy + (DRAWER_ROW - ICON) / 2, ICON);
      const int lx = left + 18 + ICON + 14;
      const int lh = TOOLS[i].hint ? bodyH + smallH : bodyH;
      const int ly = dy + (DRAWER_ROW - lh) / 2;
      renderer.drawText(UI_12_FONT_ID, lx, ly, TOOLS[i].label, true, EpdFontFamily::BOLD);
      if (TOOLS[i].hint) renderer.drawText(SMALL_FONT_ID, lx, ly + bodyH, TOOLS[i].hint);
      renderer.drawIcon(TrainChevRIcon, w - 14 - SMALL_ICON, dy + (DRAWER_ROW - SMALL_ICON) / 2, SMALL_ICON);
      dy += DRAWER_ROW;
      renderer.drawLine(left + 3, dy - 1, w - 1, dy - 1);
    }
    // Sleep without the side button.
    const int sbH = 56;
    const int sbY = h - m.buttonHintsHeight - 52 - 16 - sbH;
    sleepBox = {left + 16, sbY, w - 16, sbY + sbH};
    renderer.drawRoundedRect(left + 16, sbY, w - left - 32, sbH, 2, 12, true);
    const char* sl = "Sleep now";
    renderer.drawText(UI_12_FONT_ID,
                      left + 16 + (w - left - 32 - renderer.getTextWidth(UI_12_FONT_ID, sl, EpdFontFamily::BOLD)) / 2,
                      sbY + (sbH - bodyH) / 2, sl, true, EpdFontFamily::BOLD);
    const std::string ver = std::string("Firmware ") + CROSSPOINT_VERSION;
    renderer.drawLine(left, h - m.buttonHintsHeight - 52, w - 1, h - m.buttonHintsHeight - 52, 2, true);
    renderer.drawText(SMALL_FONT_ID, left + 18, h - m.buttonHintsHeight - 52 + (52 - smallH) / 2, ver.c_str());
  }

  const auto labels =
      drawerOpen ? mappedInput.mapLabels("Close", "Open", "Up", "Down") : mappedInput.mapLabels("Menu", "Open", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
