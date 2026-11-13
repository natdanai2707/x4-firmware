#include "CompoundActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <set>

#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/study/CompoundBriefActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "study/CompoundBrief.h"

namespace fui = freeink::ui;

namespace {
// Briefs live in a hidden folder so they never show up next to books.
constexpr char DIR[] = "/.crosspoint/compound";
constexpr char CONFIG_PATH[] = "/.crosspoint/compound/feed.txt";
constexpr char INDEX_PATH[] = "/.crosspoint/compound/index.txt";
constexpr char INDEX_TMP_PATH[] = "/.crosspoint/compound/index.txt.new";
// Earlier builds kept EPUB copies in this visible folder; it is removed.
constexpr char LEGACY_DIR[] = "/compound";
constexpr char DEFAULT_FEED_URL[] = "https://raw.githubusercontent.com/natdanai2707/compound-feed/main/feed/";
// Keep today's briefs and the two days before; older ones are deleted.
constexpr int KEEP_DAYS = 3;
constexpr size_t KEEP_WITHOUT_CLOCK = 6;
constexpr size_t MAX_CONFIG_BYTES = 1024;
constexpr size_t MAX_INDEX_BYTES = 16 * 1024;
constexpr size_t MAX_BRIEFS = 40;
constexpr char LAST_SYNC_PATH[] = "/.crosspoint/compound/last_sync.txt";
constexpr char VOCAB_PATH[] = "/study/vocab.tsv";
constexpr size_t MAX_VOCAB_BYTES = 256 * 1024;
constexpr size_t MAX_BRIEF_BYTES = 64 * 1024;
// Morning briefs are published by about 06:55 Bangkok time.
constexpr int AUTO_SYNC_HOUR = 7;
constexpr int GAP = 10;
constexpr int TAB_BAR_HEIGHT = 60;
constexpr const char* TAB_NAMES[] = {"Markets", "Ideas", "Words"};
constexpr const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string trim(std::string s) {
  const auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

// "2026-11-06" -> "6 Nov"
std::string shortDate(const std::string& iso) {
  if (iso.size() < 10) return iso;
  const int month = std::atoi(iso.substr(5, 2).c_str());
  const int day = std::atoi(iso.substr(8, 2).c_str());
  if (month < 1 || month > 12 || day < 1) return iso;
  return std::to_string(day) + " " + MONTHS[month - 1];
}

// "news-2026-11-13.epub" -> "news-2026-11-13.cb": the native brief file.
std::string cbName(const std::string& epub) { return epub.substr(0, epub.size() - 5) + ".cb"; }

// Local date as "YYYY-MM-DD" and the hour; empty when the clock is not set.
std::string localDate(int* hour) {
  const time_t now = time(nullptr);
  if (now < 1577836800) return "";
  struct tm t{};
  localtime_r(&now, &t);
  if (hour) *hour = t.tm_hour;
  char buf[40];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return buf;
}

// Oldest date still kept, "YYYY-MM-DD"; empty when the clock is not set.
std::string keepFromDate() {
  time_t now = time(nullptr);
  if (now < 1577836800) return "";
  struct tm t{};
  localtime_r(&now, &t);
  t.tm_mday -= KEEP_DAYS - 1;
  t.tm_hour = 12;  // away from DST edges
  mktime(&t);
  char buf[40];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return buf;
}

// "2026-11-13" -> "Nov 13, 2026"
std::string longDate(const std::string& iso) {
  if (iso.size() < 10) return iso;
  const int month = std::atoi(iso.substr(5, 2).c_str());
  if (month < 1 || month > 12) return iso;
  return std::string(MONTHS[month - 1]) + " " + std::to_string(std::atoi(iso.substr(8, 2).c_str())) + ", " +
         iso.substr(0, 4);
}

// A feed file name must be a plain "<name>.epub": no paths from the server.
bool safeFileName(const std::string& name) {
  if (name.size() < 6 || name.size() > 80) return false;
  if (name.compare(name.size() - 5, 5, ".epub") != 0) return false;
  return std::all_of(
             name.begin(), name.end(),
             [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'; }) &&
         name.find("..") == std::string::npos;
}
}  // namespace

std::vector<CompoundActivity::IndexEntry> CompoundActivity::parseIndex(const std::string& text) {
  // file \t type \t date \t title \t created_at \t hash
  std::vector<IndexEntry> entries;
  size_t start = 0;
  while (start < text.size() && entries.size() < MAX_BRIEFS) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.empty() || line[0] == '#') continue;
    IndexEntry e;
    size_t col = 0;
    for (int i = 0; i <= 5; ++i) {
      const size_t tab = line.find('\t', col);
      const std::string value = trim(line.substr(col, tab == std::string::npos ? std::string::npos : tab - col));
      if (i == 0) e.file = value;
      if (i == 2) e.date = value;
      if (i == 5) e.hash = value;
      if (tab == std::string::npos) break;
      col = tab + 1;
    }
    if (safeFileName(e.file)) entries.push_back(std::move(e));
  }
  return entries;
}

// Drops briefs older than KEEP_DAYS. The index is newest first.
void CompoundActivity::keepRecent(std::vector<IndexEntry>& entries) {
  const std::string from = keepFromDate();
  if (from.empty()) {
    if (entries.size() > KEEP_WITHOUT_CLOCK) entries.resize(KEEP_WITHOUT_CLOCK);
    return;
  }
  entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const IndexEntry& e) { return e.date < from; }),
                entries.end());
}

// Deletes every brief file not in `keep`, plus the old visible folder.
void CompoundActivity::pruneFiles(const std::vector<IndexEntry>& keep) {
  for (const auto& name : Storage.listFiles(DIR, 200)) {
    const std::string file = name.c_str();
    const bool isCb = file.size() > 3 && file.compare(file.size() - 3, 3, ".cb") == 0;
    if (!isCb && !safeFileName(file)) continue;
    const bool kept =
        isCb && std::any_of(keep.begin(), keep.end(), [&](const IndexEntry& e) { return cbName(e.file) == file; });
    if (!kept) Storage.remove((std::string(DIR) + "/" + file).c_str());
  }
}

// Moves an old /compound/feed.txt into the hidden folder, then removes /compound.
void CompoundActivity::migrateLegacy() {
  if (!Storage.exists(LEGACY_DIR)) return;
  Storage.ensureDirectoryExists(DIR);
  const std::string oldConfig = std::string(LEGACY_DIR) + "/feed.txt";
  std::string text;
  if (!Storage.exists(CONFIG_PATH) && Storage.exists(oldConfig.c_str()) &&
      Storage.readFileToString("COMPOUND", oldConfig, MAX_CONFIG_BYTES, text) &&
      text.find("token=") != std::string::npos) {
    Storage.writeFile(CONFIG_PATH, String(text.c_str()));  // keep a private feed token
  }
  if (!Storage.removeDir(LEGACY_DIR)) LOG_ERR("COMPOUND", "Could not remove %s", LEGACY_DIR);
}

CompoundActivity::CompoundActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Compound", renderer, mappedInput) {}

void CompoundActivity::onEnter() {
  Activity::onEnter();
  migrateLegacy();
  readConfig();
  {
    // Age out old briefs even without a network.
    std::string text;
    if (Storage.exists(INDEX_PATH)) Storage.readFileToString("COMPOUND", INDEX_PATH, MAX_INDEX_BYTES, text);
    auto entries = parseIndex(text);
    keepRecent(entries);
    pruneFiles(entries);
  }
  thaiFont = makeUniqueNoThrow<StudyFont>(renderer, StudyFont::Script::Thai);
  if (thaiFont && !thaiFont->load()) thaiFont.reset();
  loadBriefs();
  layoutWords();
  autoSyncPending = shouldAutoSync();
  requestUpdate();
}

void CompoundActivity::onExit() {
  stopWifi();
  Activity::onExit();
  briefs.clear();
  wordLines.clear();
  wordPages.clear();
  thaiFont.reset();
}

// Once a day after AUTO_SYNC_HOUR, sync on open when a WiFi network is saved.
bool CompoundActivity::shouldAutoSync() {
  if (feedUrl.empty()) return false;
  int hour = 0;
  const std::string today = localDate(&hour);
  if (today.empty() || hour < AUTO_SYNC_HOUR) return false;
  WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getCredentialCount() == 0 && WIFI_STORE.getLastConnectedSsid().empty()) return false;
  std::string last;
  if (Storage.exists(LAST_SYNC_PATH)) Storage.readFileToString("COMPOUND", LAST_SYNC_PATH, 64, last);
  return trim(last) != today;
}

bool CompoundActivity::readConfig() {
  feedUrl = DEFAULT_FEED_URL;
  token.clear();
  std::string text;
  if (!Storage.exists(CONFIG_PATH) || !Storage.readFileToString("COMPOUND", CONFIG_PATH, MAX_CONFIG_BYTES, text)) {
    return true;
  }
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = trim(text.substr(start, end - start));
    start = end + 1;
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));
    if (key == "url") feedUrl = value;
    if (key == "token") token = value;
  }
  if (!feedUrl.empty() && feedUrl.back() != '/') feedUrl += '/';
  return !feedUrl.empty();
}

void CompoundActivity::loadBriefs() {
  briefs.clear();
  std::string text;
  if (!Storage.exists(INDEX_PATH) || !Storage.readFileToString("COMPOUND", INDEX_PATH, MAX_INDEX_BYTES, text)) return;
  for (const auto& e : parseIndex(text)) {
    Brief b;
    b.path = std::string(DIR) + "/" + cbName(e.file);
    std::string body;
    if (!Storage.exists(b.path.c_str()) || !Storage.readFileToString("COMPOUND", b.path, MAX_BRIEF_BYTES, body))
      continue;
    b.data = CompoundBrief::parse(body);
    if (b.data.empty()) continue;
    // The list needs title, hook, market chips and terms; the reader reloads the rest.
    b.data.sections.clear();
    b.data.sections.shrink_to_fit();
    b.data.followups.clear();
    briefs.push_back(std::move(b));
  }
  if (syncStatus.empty()) syncStatus = briefs.empty() ? "Tap to sync" : "Tap to sync";
}

std::vector<const CompoundActivity::Brief*> CompoundActivity::ofTab() const {
  std::vector<const Brief*> out;
  const bool news = tab == Tab::Markets;
  for (const auto& b : briefs) {
    if (b.data.isNews() == news) out.push_back(&b);
  }
  return out;
}

int CompoundActivity::contentTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight + GAP;
}

int CompoundActivity::tabBarTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - m.buttonHintsHeight - TAB_BAR_HEIGHT;
}

void CompoundActivity::setTab(const Tab t) {
  {
    RenderLock lock(*this);
    tab = t;
    focus = 0;
    wordPage = 0;
  }
  requestUpdate();
}

void CompoundActivity::openBrief(const Brief* b) {
  if (!b) return;
  auto screen = makeUniqueNoThrow<CompoundBriefActivity>(renderer, mappedInput, b->path);
  if (!screen) {
    LOG_ERR("COMPOUND", "OOM: brief screen");
    return;
  }
  startActivityForResult(std::move(screen), [this](const ActivityResult&) { requestUpdate(); });
}

// Words tab, like the phone app: every term from the briefs on the device.
void CompoundActivity::layoutWords() {
  wordLines.clear();
  wordPages.clear();
  wordPage = 0;
  const auto& m = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - 2 * m.contentSidePadding;
  const auto add = [&](const std::string& text, int latin, uint8_t style, int indent, int gap, bool rule, bool bar) {
    if (text.empty()) return;
    const int fontId = thaiFont && StudyFont::needsThai(text) ? thaiFont->bodyFontId() : latin;
    const uint8_t s = fontId == latin ? style : EpdFontFamily::REGULAR;
    bool first = true;
    for (auto& w :
         renderer.wrappedText(fontId, text.c_str(), width - indent, 30, static_cast<EpdFontFamily::Style>(s))) {
      wordLines.push_back({fontId, s, indent, first ? gap : 0, first && rule, bar, std::move(w)});
      first = false;
    }
  };
  std::set<std::string> seen;
  int count = 0;
  for (const auto& b : briefs) {
    for (const auto& t : b.data.terms) {
      std::string key = trim(t.en);
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
      if (key.empty() || !seen.insert(key).second) continue;
      ++count;
      add(t.en, UI_12_FONT_ID, EpdFontFamily::BOLD, 0, GAP + 4, true, false);
      add(t.th, UI_12_FONT_ID, EpdFontFamily::REGULAR, 0, 0, false, false);
      add(t.explain, UI_12_FONT_ID, EpdFontFamily::REGULAR, 0, 4, false, false);
      if (!t.example.empty()) add("\"" + t.example + "\"", UI_12_FONT_ID, EpdFontFamily::ITALIC, 16, 6, false, true);
      add("From: " + b.data.title, SMALL_FONT_ID, EpdFontFamily::REGULAR, 0, 6, false, false);
    }
  }
  if (count == 0)
    add("Words from every brief collect here.", UI_12_FONT_ID, EpdFontFamily::REGULAR, 0, 0, false, false);
  // First page starts below the title block.
  const int firstTop =
      contentTop() + renderer.getLineHeight(NOTOSANS_18_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + GAP;
  const int bottom = tabBarTop() - GAP;
  int y = firstTop;
  wordPages.push_back(0);
  for (size_t i = 0; i < wordLines.size(); ++i) {
    const int lh = renderer.getLineHeight(wordLines[i].fontId);
    const int h = lh + (i == wordPages.back() ? 0 : wordLines[i].gapBefore);
    if (y + h > bottom && i != wordPages.back()) {
      wordPages.push_back(i);
      y = firstTop + lh;
      continue;
    }
    y += h;
  }
}

void CompoundActivity::loop() {
  using B = MappedInputManager::Button;
  if (autoSyncPending) {
    autoSyncPending = false;
    startSync();
    return;
  }
  if (syncing) return;
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
  if (tab == Tab::Words) {
    const int pages = static_cast<int>(wordPages.size());
    int next = wordPage;
    if (down || mappedInput.wasReleased(B::Confirm)) next = std::min(pages - 1, wordPage + 1);
    if (up) next = std::max(0, wordPage - 1);
    if (next != wordPage) {
      {
        RenderLock lock(*this);
        wordPage = next;
      }
      requestUpdate();
    }
  } else {
    const auto items = ofTab();
    const int count = static_cast<int>(items.size());
    if ((down || up) && count > 0) {
      {
        RenderLock lock(*this);
        focus = (focus + (down ? 1 : count - 1)) % count;
      }
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(B::Confirm) && focus < count) return openBrief(items[focus]);
  }

  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return;
  if (y >= tabBarTop()) {
    const int t = x * TAB_COUNT / std::max(1, renderer.getScreenWidth());
    return setTab(static_cast<Tab>(std::min(t, TAB_COUNT - 1)));
  }
  if (y >= statusTop && y < statusBottom) return startSync();
  if (tab == Tab::Words) {
    const int pages = static_cast<int>(wordPages.size());
    const int next = x >= renderer.getScreenWidth() / 2 ? std::min(pages - 1, wordPage + 1) : std::max(0, wordPage - 1);
    if (next != wordPage) {
      {
        RenderLock lock(*this);
        wordPage = next;
      }
      requestUpdate();
    }
    return;
  }
  const auto items = ofTab();
  for (size_t i = 0; i < itemTops.size() && i < items.size(); ++i) {
    if (y >= itemTops[i] && y < itemBottoms[i]) return openBrief(items[i]);
  }
}

void CompoundActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;

  std::string today;
  const time_t now = time(nullptr);
  if (now > 1577836800) {
    struct tm t{};
    localtime_r(&now, &t);
    static constexpr const char* DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    today = std::string(DAYS[t.tm_wday]) + " " + std::to_string(t.tm_mday) + " " + MONTHS[t.tm_mon];
  }
  GUI.drawHeader(renderer, Rect{0, m.topPadding, w, m.headerHeight}, "COMPOUND.",
                 today.empty() ? nullptr : today.c_str());

  // Section title with the sync status on the right (tap it to sync).
  int y = contentTop();
  renderer.drawText(NOTOSANS_18_FONT_ID, pad, y, TAB_NAMES[static_cast<int>(tab)], true, EpdFontFamily::BOLD);
  const std::string status = syncing ? "Syncing..." : syncStatus;
  const int sw = renderer.getTextWidth(SMALL_FONT_ID, status.c_str());
  const int sy = y + renderer.getLineHeight(NOTOSANS_18_FONT_ID) - renderer.getLineHeight(SMALL_FONT_ID) - 4;
  renderer.drawText(SMALL_FONT_ID, w - pad - sw, sy, status.c_str());
  statusTop = y;
  statusBottom = y + renderer.getLineHeight(NOTOSANS_18_FONT_ID);
  y += renderer.getLineHeight(NOTOSANS_18_FONT_ID);

  if (tab == Tab::Words) {
    renderWords(y);
  } else {
    renderFeed(y + GAP);
  }
  renderTabBar();
  renderer.displayBuffer();
}

void CompoundActivity::renderFeed(int y) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;
  const int width = w - 2 * pad;
  const int bottom = tabBarTop() - GAP;
  const auto items = ofTab();
  itemTops.clear();
  itemBottoms.clear();
  const bool news = tab == Tab::Markets;

  if (items.empty()) {
    renderer.drawLine(pad, y, w - pad, y, 2, true);
    y += GAP * 2;
    renderer.drawText(UI_12_FONT_ID, pad, y, news ? "No market brief yet" : "No idea brief yet", true,
                      EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, pad, y,
                      news ? "A new brief arrives every morning. Tap Sync." : "A new idea arrives every morning.");
    return;
  }

  // Hero: the latest brief as a card. Everything inside keeps the same inset.
  constexpr int IN = 16;
  const auto& hero = items[0]->data;
  const int heroTop = y;
  const int inner = width - 2 * IN;
  const int x = pad + IN;
  y += IN;
  const std::string meta = std::string(news ? "MARKETS" : "IDEAS") + "  ·  " + longDate(hero.date) + "  ·  5 min read";
  renderer.drawText(SMALL_FONT_ID, x, y, meta.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 6;
  for (const auto& l : renderer.wrappedText(NOTOSANS_16_FONT_ID, hero.title.c_str(), inner, 3, EpdFontFamily::BOLD)) {
    renderer.drawText(NOTOSANS_16_FONT_ID, x, y, l.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(NOTOSANS_16_FONT_ID);
  }
  y += GAP;
  if (news && !hero.metrics.empty()) {
    // 3 x 2 grid of market chips: label, then change.
    const int cols = 3;
    const int cellW = inner / cols;
    const int cellH = renderer.getLineHeight(SMALL_FONT_ID) + renderer.getLineHeight(UI_12_FONT_ID) + 14;
    const int n = std::min<int>(6, hero.metrics.size());
    const int rows = (n + cols - 1) / cols;
    renderer.drawRect(x, y, cellW * cols, cellH * rows);
    for (int i = 0; i < n; ++i) {
      const int cx = x + (i % cols) * cellW;
      const int cy = y + (i / cols) * cellH;
      if (i % cols) renderer.drawLine(cx, cy, cx, cy + cellH - 1);
      if (i >= cols && i % cols == 0) renderer.drawLine(x, cy, x + cellW * cols - 1, cy);
      const auto& mt = hero.metrics[i];
      renderer.drawText(SMALL_FONT_ID, cx + 10, cy + 6, mt.label.c_str());
      const std::string v = mt.change.empty() ? mt.value : mt.change;
      renderer.drawText(UI_12_FONT_ID, cx + 10, cy + 6 + renderer.getLineHeight(SMALL_FONT_ID), v.c_str(), true,
                        EpdFontFamily::BOLD);
    }
    y += cellH * rows + GAP + 4;
  } else if (!hero.hook.empty()) {
    for (const auto& l : renderer.wrappedText(UI_12_FONT_ID, hero.hook.c_str(), inner, 3, EpdFontFamily::ITALIC)) {
      renderer.drawText(UI_12_FONT_ID, x, y, l.c_str(), true, EpdFontFamily::ITALIC);
      y += renderer.getLineHeight(UI_12_FONT_ID);
    }
    y += GAP + 4;
  }
  // Read button: filled, like the phone's primary action.
  const int btnH = 44;
  const int btnW = 120;
  renderer.fillRoundedRect(x, y, btnW, btnH, 6, Color::Black);
  const int tw = renderer.getTextWidth(UI_12_FONT_ID, "Read", EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + (btnW - tw) / 2, y + (btnH - renderer.getLineHeight(UI_12_FONT_ID)) / 2, "Read",
                    false, EpdFontFamily::BOLD);
  y += btnH + IN;
  renderer.drawRoundedRect(pad, heroTop, width, y - heroTop, focus == 0 ? 3 : 1, 12, true);
  itemTops.push_back(heroTop);
  itemBottoms.push_back(y);
  y += GAP + 8;

  if (items.size() < 2) return;
  renderer.drawText(SMALL_FONT_ID, pad, y, "EARLIER", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 2;
  renderer.drawLine(pad, y, w - pad, y);
  for (size_t i = 1; i < items.size(); ++i) {
    const auto& b = items[i]->data;
    const int top = y;
    const int dateW = 52;
    const int titleW = width - dateW - 12;
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, b.title.c_str(), titleW, 1);
    const int rowH = std::max<int>(
        66, lines.size() * renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 16);
    if (top + rowH > bottom) break;
    // Date block: day number over month.
    const std::string day = b.date.size() >= 10 ? std::to_string(std::atoi(b.date.substr(8, 2).c_str())) : "";
    const std::string mon =
        b.date.size() >= 10 ? MONTHS[std::max(0, std::min(11, std::atoi(b.date.substr(5, 2).c_str()) - 1))] : "";
    const int dw = renderer.getTextWidth(NOTOSANS_16_FONT_ID, day.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(NOTOSANS_16_FONT_ID, pad + (dateW - dw) / 2, top + 4, day.c_str(), true, EpdFontFamily::BOLD);
    const int mw = renderer.getTextWidth(SMALL_FONT_ID, mon.c_str());
    renderer.drawText(SMALL_FONT_ID, pad + (dateW - mw) / 2, top + renderer.getLineHeight(NOTOSANS_16_FONT_ID) - 3,
                      mon.c_str());
    int ty = top + 10;
    for (const auto& l : lines) {
      renderer.drawText(UI_12_FONT_ID, pad + dateW + 12, ty, l.c_str());
      ty += renderer.getLineHeight(UI_12_FONT_ID);
    }
    renderer.drawText(SMALL_FONT_ID, pad + dateW + 12, ty, news ? "Market brief" : "Idea");
    y = top + rowH;
    renderer.drawLine(pad, y, w - pad, y);
    // Focus: a bar on the left edge keeps the row text clear of any outline.
    if (focus == static_cast<int>(i)) renderer.fillRect(pad - 10, top + 8, 4, rowH - 16, true);
    itemTops.push_back(top);
    itemBottoms.push_back(y);
  }
}

void CompoundActivity::renderWords(int y) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pad = m.contentSidePadding;
  {
    std::set<std::string> seen;
    for (const auto& b : briefs) {
      for (const auto& t : b.data.terms) seen.insert(t.en);
    }
    const std::string sub = std::to_string(seen.size()) + " terms from your briefs";
    renderer.drawText(SMALL_FONT_ID, pad, y, sub.c_str());
    y += renderer.getLineHeight(SMALL_FONT_ID) + GAP;
  }
  // Page n of m above the tab bar.
  if (wordPages.size() > 1) {
    const std::string pg = std::to_string(wordPage + 1) + " / " + std::to_string(wordPages.size());
    const int pw = renderer.getTextWidth(SMALL_FONT_ID, pg.c_str());
    renderer.drawText(SMALL_FONT_ID, w - pad - pw, tabBarTop() - GAP - renderer.getLineHeight(SMALL_FONT_ID),
                      pg.c_str());
  }
  if (wordPages.empty()) return;
  const size_t from = wordPages[wordPage];
  const size_t to = wordPage + 1 < static_cast<int>(wordPages.size()) ? wordPages[wordPage + 1] : wordLines.size();
  for (size_t i = from; i < to; ++i) {
    const auto& l = wordLines[i];
    if (i != from) y += l.gapBefore;
    const int lh = renderer.getLineHeight(l.fontId);
    if (l.rule) renderer.drawLine(pad, y - 4, w - pad, y - 4);
    if (l.bar) renderer.fillRect(pad, y, 3, lh, true);
    renderer.drawText(l.fontId, pad + l.indent, y, l.text.c_str(), true, static_cast<EpdFontFamily::Style>(l.style));
    y += lh;
  }
}

void CompoundActivity::renderTabBar() {
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

void CompoundActivity::startSync() {
  if (!readConfig()) {
    {
      RenderLock lock(*this);
      syncStatus = "No feed url";
    }
    requestUpdate();
    return;
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             stopWifi();
                             return;
                           }
                           runSync();
                         });
}

void CompoundActivity::runSync() {
  {
    RenderLock lock(*this);
    syncing = true;
    syncStatus = "Syncing...";
  }
  requestUpdateAndWait();

  std::vector<HttpDownloader::Header> headers;
  if (!token.empty()) headers.emplace_back("Authorization", "Bearer " + token);
  Storage.ensureDirectoryExists(DIR);

  int added = 0;
  int failed = 0;
  std::vector<std::string> newBriefs;
  bool indexOk = false;
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("COMPOUND", "Low heap for sync");
  } else if (HttpDownloader::downloadToFile(feedUrl + "index.txt", INDEX_TMP_PATH, nullptr, nullptr, "", "", headers) ==
             HttpDownloader::OK) {
    indexOk = true;
    std::string text;
    Storage.readFileToString("COMPOUND", INDEX_TMP_PATH, MAX_INDEX_BYTES, text);
    std::vector<IndexEntry> wanted = parseIndex(text);
    keepRecent(wanted);
    // Hashes from the previous sync: a brief whose hash changed is downloaded again.
    std::string oldText;
    if (Storage.exists(INDEX_PATH)) Storage.readFileToString("COMPOUND", INDEX_PATH, MAX_INDEX_BYTES, oldText);
    const std::vector<IndexEntry> previous = parseIndex(oldText);
    for (const auto& entry : wanted) {
      const std::string cb = cbName(entry.file);
      const std::string dest = std::string(DIR) + "/" + cb;
      const auto old =
          std::find_if(previous.begin(), previous.end(), [&](const IndexEntry& e) { return e.file == entry.file; });
      const bool changed = old == previous.end() || old->hash != entry.hash;
      if (Storage.exists(dest.c_str()) && !changed) continue;
      if (HttpDownloader::downloadToFile(feedUrl + cb, dest, nullptr, nullptr, "", "", headers) == HttpDownloader::OK) {
        ++added;
        newBriefs.push_back(dest);
      } else {
        ++failed;
        LOG_ERR("COMPOUND", "Download failed: %s", entry.file.c_str());
      }
    }
    Storage.replaceFile(INDEX_TMP_PATH, INDEX_PATH);
    pruneFiles(wanted);
  } else {
    LOG_ERR("COMPOUND", "Index download failed (HTTP %d)", HttpDownloader::lastStatus());
  }
  stopWifi();
  int wordsAdded = 0;
  if (indexOk) {
    wordsAdded = addWordsToVocab(newBriefs);
    const std::string today = localDate(nullptr);
    if (!today.empty()) Storage.writeFile(LAST_SYNC_PATH, String(today.c_str()));
  }

  {
    RenderLock lock(*this);
    syncing = false;
    loadBriefs();
    if (!indexOk) {
      const int status = HttpDownloader::lastStatus();
      syncStatus = status == 401 || status == 403 || status == 404
                       ? "Feed refused (HTTP " + std::to_string(status) + "): check url and token"
                       : "Sync failed: no connection to the feed";
    } else if (failed > 0) {
      syncStatus = std::to_string(added) + " new, " + std::to_string(failed) + " failed";
    } else {
      syncStatus = added == 0 ? "Up to date" : std::to_string(added) + " new or updated briefs";
      if (wordsAdded > 0) syncStatus += ", " + std::to_string(wordsAdded) + " words to IELTS";
    }
    layoutWords();
  }
  requestUpdate();
}

void CompoundActivity::stopWifi() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
}

// Appends the Words of new briefs to the IELTS deck, skipping words already in
// it. New cards go at the end so saved review progress still matches.
int CompoundActivity::addWordsToVocab(const std::vector<std::string>& briefPaths) {
  if (briefPaths.empty()) return 0;
  std::string deck;
  if (Storage.exists(VOCAB_PATH)) Storage.readFileToString("COMPOUND", VOCAB_PATH, MAX_VOCAB_BYTES, deck);
  std::set<std::string> known;
  size_t start = 0;
  while (start < deck.size()) {
    size_t end = deck.find('\n', start);
    if (end == std::string::npos) end = deck.size();
    const std::string line = deck.substr(start, end - start);
    start = end + 1;
    if (line.empty() || line[0] == '#') continue;
    std::string front = trim(line.substr(0, line.find('\t')));
    std::transform(front.begin(), front.end(), front.begin(), [](unsigned char c) { return std::tolower(c); });
    known.insert(front);
  }
  std::string extra;
  int count = 0;
  const auto clean = [](std::string v) {
    std::replace(v.begin(), v.end(), '\t', ' ');
    std::replace(v.begin(), v.end(), '\n', ' ');
    return v;
  };
  for (const auto& path : briefPaths) {
    std::string text;
    if (!Storage.readFileToString("COMPOUND", path, MAX_BRIEF_BYTES, text)) continue;
    for (const auto& t : CompoundBrief::parse(text).terms) {
      std::string key = trim(t.en);
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
      if (key.empty() || !known.insert(key).second) continue;
      // word <TAB> meaning <TAB> example <TAB> Thai
      extra += clean(t.en) + "\t" + clean(t.explain) + "\t" + clean(t.example) + "\t" + clean(t.th) + "\n";
      ++count;
    }
  }
  if (count == 0) return 0;
  if (!deck.empty() && deck.back() != '\n') deck += '\n';
  deck += extra;
  Storage.ensureDirectoryExists("/study");
  if (!Storage.writeFile(VOCAB_PATH, String(deck.c_str()))) {
    LOG_ERR("COMPOUND", "Could not update %s", VOCAB_PATH);
    return 0;
  }
  return count;
}
