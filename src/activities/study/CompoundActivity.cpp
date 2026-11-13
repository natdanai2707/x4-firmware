#include "CompoundActivity.h"

#include <HalStorage.h>
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
    : UiListActivity("Compound", renderer, mappedInput) {}

void CompoundActivity::onEnter() {
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
  loadBriefs();
  rebuildRows();
  UiListActivity::onEnter();
  autoSyncPending = shouldAutoSync();
}

void CompoundActivity::loop() {
  if (autoSyncPending) {
    autoSyncPending = false;
    startSync();
    return;
  }
  UiListActivity::loop();
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

void CompoundActivity::onExit() {
  stopWifi();
  UiListActivity::onExit();
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
  size_t start = 0;
  while (start < text.size() && briefs.size() < MAX_BRIEFS) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.empty() || line[0] == '#') continue;
    // file \t type \t date \t title [\t created_at]
    std::vector<std::string> cols;
    size_t col = 0;
    while (cols.size() < 5) {
      const size_t tab = line.find('\t', col);
      cols.push_back(trim(line.substr(col, tab == std::string::npos ? std::string::npos : tab - col)));
      if (tab == std::string::npos) break;
      col = tab + 1;
    }
    if (cols.size() < 4 || !safeFileName(cols[0])) continue;
    if (!Storage.exists((std::string(DIR) + "/" + cbName(cols[0])).c_str())) continue;
    Brief b;
    b.file = cols[0];
    b.type = cols[1];
    b.date = cols[2];
    b.title = cols[3];
    b.subtitle = (b.type == "news" ? std::string("Markets") : std::string("Idea")) + " · " + shortDate(b.date);
    briefs.push_back(std::move(b));
  }
}

void CompoundActivity::rebuildRows() {
  if (feedUrl.empty()) {
    syncLabel = "Set up sync";
    if (syncStatus.empty()) syncStatus = "Add /compound/feed.txt with the feed url";
  } else {
    syncLabel = "Sync now";
    if (syncStatus.empty()) {
      syncStatus = briefs.empty() ? "Download the latest briefs over WiFi"
                                  : std::to_string(briefs.size()) + " briefs on this device";
    }
  }
  rows.clear();
  rows.reserve(briefs.size() + 1);
  fui::ListItem sync;
  sync.label = syncLabel.c_str();
  sync.subtitle = syncStatus.c_str();
  sync.icon = listIconFor(UIIcon::Wifi, 32);
  sync.actionValue = 0;
  rows.push_back(sync);
  for (size_t i = 0; i < briefs.size(); ++i) {
    fui::ListItem item;
    item.label = briefs[i].title.c_str();
    item.subtitle = briefs[i].subtitle.c_str();
    item.icon = listIconFor(UIIcon::Book, 32);
    item.actionValue = static_cast<int16_t>(i + 1);
    rows.push_back(item);
  }
}

void CompoundActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}

void CompoundActivity::activateIndex(const int index) {
  if (syncing) return;
  app.clearTapFlash();
  nav.selected = index;
  if (index == 0) {
    startSync();
    return;
  }
  const size_t b = static_cast<size_t>(index - 1);
  if (b >= briefs.size()) return;
  auto screen =
      makeUniqueNoThrow<CompoundBriefActivity>(renderer, mappedInput, std::string(DIR) + "/" + cbName(briefs[b].file));
  if (!screen) {
    LOG_ERR("COMPOUND", "OOM: brief screen");
    return;
  }
  startActivityForResult(std::move(screen), nullptr);
}

void CompoundActivity::onBackButton() { finish(); }

void CompoundActivity::startSync() {
  if (!readConfig()) {
    {
      RenderLock lock(*this);
      syncStatus = "No feed url: add /compound/feed.txt";
      rebuildRows();
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
    rebuildRows();
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
    rebuildRows();
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
