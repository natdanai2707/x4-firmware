#include "LauncherActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
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
#include "components/icons/transfer.h"
#include "fontIds.h"

namespace {
constexpr int COLS = 2;
constexpr int GRID_GAP = 18;
constexpr int ICON = 32;       // drawer icons
constexpr int TILE_ICON = 64;  // 8-bit app icons
constexpr int EDGE = 40;       // right-edge band that starts the drawer swipe
constexpr int DRAWER_ROW = 72;
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
    {"IELTS", "Vocab review", PixelVocabIcon},
    {"Chinese", "Hanzi review", PixelChineseIcon},
    {"Training", "Run, gym, grip", PixelTrainIcon},
};
constexpr Tile TOOLS[] = {
    {"Browse Files", nullptr, FolderIcon},
    {"Library", nullptr, LibraryIcon},
    {"File Transfer", "USB, WiFi", TransferIcon},
    {"Settings", nullptr, Settings2Icon},
};
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
  measureCard();
  requestUpdate();
}

int LauncherActivity::cardTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  // Below the clock line (see render()).
  return m.topPadding + m.headerHeight + SECTION_GAP / 2 + renderer.getLineHeight(NOTOSANS_18_FONT_ID) + SECTION_GAP;
}

void LauncherActivity::measureCard() {
  const auto& m = UITheme::getInstance().getMetrics();
  const int inner = renderer.getScreenWidth() - 2 * m.contentSidePadding - 2 * INSET;
  titleLines.clear();
  if (!bookPath.empty()) {
    titleLines = renderer.wrappedText(UI_12_FONT_ID, bookTitle.c_str(), inner, 2, EpdFontFamily::BOLD);
  }
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  int h = INSET + smallH + 8;
  if (bookPath.empty()) {
    h += bodyH + 4 + smallH;
  } else {
    h += static_cast<int>(titleLines.size()) * bodyH;
    if (!bookAuthor.empty()) h += 4 + smallH;
  }
  cardH = h + INSET;
}

// Two rows of tiles share the space below the card, within a pleasant ratio.
int LauncherActivity::tileHeight() const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int size = tileSize();
  const int bottom = renderer.getScreenHeight() - m.buttonHintsHeight - SECTION_GAP;
  const int fit = (bottom - gridTop() - GRID_GAP) / 2;
  return std::max(size / 2, std::min(fit, size * 9 / 10));
}

int LauncherActivity::tileSize() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return (renderer.getScreenWidth() - 2 * m.contentSidePadding - (COLS - 1) * GRID_GAP) / COLS;
}

int LauncherActivity::drawerLeft() const { return renderer.getScreenWidth() * 2 / 5; }

int LauncherActivity::tileAt(const int x, const int y) const {
  const auto& m = UITheme::getInstance().getMetrics();
  if (y >= cardTop() && y < cardTop() + cardHeight()) return 0;
  const int size = tileSize();
  const int tileH = tileHeight();
  if (y < gridTop() || x < m.contentSidePadding) return -1;
  const int col = (x - m.contentSidePadding) / (size + GRID_GAP);
  const int row = (y - gridTop()) / (tileH + GRID_GAP);
  if (col >= COLS) return -1;
  const int index = row * COLS + col;
  return index < APP_COUNT - 1 ? index + 1 : -1;
}

int LauncherActivity::toolAt(const int x, const int y) const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int top = m.topPadding + m.headerHeight + 16;
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
      const int t = toolAt(x, y);
      if (t >= 0) openTool(static_cast<Tool>(t));
    }
    return;
  }

  const int first = bookPath.empty() ? 1 : 0;
  int next = selected;
  if (mappedInput.wasReleased(B::Right)) next = selected + 1;
  if (mappedInput.wasReleased(B::Left)) next = selected - 1;
  if (mappedInput.wasReleased(B::Down)) next = selected == 0 ? 1 : selected + COLS;
  if (mappedInput.wasReleased(B::Up)) next = selected <= COLS ? first : selected - COLS;
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
    const int t = tileAt(x, y);
    if (t > 0 || (t == 0 && !bookPath.empty())) openApp(static_cast<App>(t));
  }
}

void LauncherActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;

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
  // Clock, with the menu hint on the same line.
  const int clockTop = m.topPadding + m.headerHeight + SECTION_GAP / 2;
  const int clockH = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  if (!clock.empty()) {
    renderer.drawText(NOTOSANS_18_FONT_ID, pad, clockTop, clock.c_str(), true, EpdFontFamily::BOLD);
  }
  static constexpr const char* HINT = "Swipe from edge for menu  <";
  renderer.drawText(SMALL_FONT_ID, w - pad - renderer.getTextWidth(SMALL_FONT_ID, HINT),
                    clockTop + (clockH - renderer.getLineHeight(SMALL_FONT_ID)) / 2, HINT);

  // Continue reading card: label, title (up to 2 lines), author.
  const int top = cardTop();
  const bool cardSel = selected == 0;
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int bodyH = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawRoundedRect(pad, top, w - 2 * pad, cardHeight(), cardSel ? 3 : 1, 12, true);
  int y = top + INSET;
  renderer.drawText(SMALL_FONT_ID, pad + INSET, y, "CONTINUE READING", true, EpdFontFamily::BOLD);
  y += smallH + 8;
  if (bookPath.empty()) {
    renderer.drawText(UI_12_FONT_ID, pad + INSET, y, "No book open yet.", true, EpdFontFamily::BOLD);
    y += bodyH + 4;
    renderer.drawText(SMALL_FONT_ID, pad + INSET, y, "Open one from Library.");
  } else {
    for (const auto& l : titleLines) {
      renderer.drawText(UI_12_FONT_ID, pad + INSET, y, l.c_str(), true, EpdFontFamily::BOLD);
      y += bodyH;
    }
    if (!bookAuthor.empty()) {
      const auto author = renderer.wrappedText(SMALL_FONT_ID, bookAuthor.c_str(), w - 2 * pad - 2 * INSET, 1);
      if (!author.empty()) renderer.drawText(SMALL_FONT_ID, pad + INSET, y + 4, author[0].c_str());
    }
  }

  // App tiles: icon top left, name and hint at the bottom, same inset.
  const int size = tileSize();
  const int tileH = tileHeight();
  for (int i = 0; i < APP_COUNT - 1; ++i) {
    const int col = i % COLS;
    const int row = i / COLS;
    const int x = pad + col * (size + GRID_GAP);
    const int ty = gridTop() + row * (tileH + GRID_GAP);
    const bool sel = selected == i + 1;
    renderer.drawRoundedRect(x, ty, size, tileH, sel ? 3 : 1, 12, true);
    renderer.drawIcon(TILES[i].icon, x + INSET, ty + INSET, TILE_ICON);
    const int hintY = ty + tileH - INSET - smallH;
    renderer.drawText(SMALL_FONT_ID, x + INSET, hintY, TILES[i].hint);
    renderer.drawText(UI_12_FONT_ID, x + INSET, hintY - bodyH, TILES[i].label, true, EpdFontFamily::BOLD);
  }

  // Device drawer.
  if (drawerOpen) {
    const int left = drawerLeft();
    const int dTop = m.topPadding + m.headerHeight;
    const int h = renderer.getScreenHeight();
    renderer.fillRect(left, dTop, w - left, h - dTop, false);
    renderer.fillRect(left, dTop, 3, h - dTop, true);
    int y = dTop + 16;
    for (int i = 0; i < TOOL_COUNT; ++i) {
      const bool sel = i == drawerSelected;
      // Outline, not fill: the icons are black-only bitmaps.
      if (sel) renderer.drawRoundedRect(left + 10, y + 4, w - left - 18, DRAWER_ROW - 8, 3, 10, true);
      renderer.drawIcon(TOOLS[i].icon, left + 18, y + (DRAWER_ROW - ICON) / 2, ICON);
      const int ty = y + (DRAWER_ROW - renderer.getLineHeight(UI_12_FONT_ID)) / 2 - (TOOLS[i].hint ? 10 : 0);
      renderer.drawText(UI_12_FONT_ID, left + 18 + ICON + 14, ty, TOOLS[i].label, true, EpdFontFamily::BOLD);
      if (TOOLS[i].hint) {
        renderer.drawText(SMALL_FONT_ID, left + 18 + ICON + 14, ty + renderer.getLineHeight(UI_12_FONT_ID),
                          TOOLS[i].hint);
      }
      y += DRAWER_ROW;
      renderer.drawLine(left + 3, y - 1, w - 1, y - 1);
    }
  }

  const auto labels =
      drawerOpen ? mappedInput.mapLabels("Close", "Open", "Up", "Down") : mappedInput.mapLabels("Menu", "Open", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
