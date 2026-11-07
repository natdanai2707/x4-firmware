#include "CompoundActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {
constexpr char DIR[] = "/compound";
constexpr char CONFIG_PATH[] = "/compound/feed.txt";
constexpr char INDEX_PATH[] = "/compound/index.txt";
constexpr char INDEX_TMP_PATH[] = "/compound/index.txt.new";
constexpr size_t MAX_CONFIG_BYTES = 1024;
constexpr size_t MAX_INDEX_BYTES = 16 * 1024;
constexpr size_t MAX_BRIEFS = 40;
constexpr const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

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

// A feed file name must be a plain "<name>.epub": no paths from the server.
bool safeFileName(const std::string& name) {
  if (name.size() < 6 || name.size() > 80) return false;
  if (name.compare(name.size() - 5, 5, ".epub") != 0) return false;
  return std::all_of(name.begin(), name.end(),
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
      if (i == 5) e.hash = value;
      if (tab == std::string::npos) break;
      col = tab + 1;
    }
    if (safeFileName(e.file)) entries.push_back(std::move(e));
  }
  return entries;
}

CompoundActivity::CompoundActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Compound", renderer, mappedInput) {}

void CompoundActivity::onEnter() {
  readConfig();
  loadBriefs();
  rebuildRows();
  UiListActivity::onEnter();
}

void CompoundActivity::onExit() {
  stopWifi();
  UiListActivity::onExit();
}

bool CompoundActivity::readConfig() {
  feedUrl.clear();
  token.clear();
  std::string text;
  if (!Storage.exists(CONFIG_PATH) || !Storage.readFileToString("COMPOUND", CONFIG_PATH, MAX_CONFIG_BYTES, text)) {
    return false;
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
    if (!Storage.exists((std::string(DIR) + "/" + cols[0]).c_str())) continue;
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
  activityManager.goToReader(std::string(DIR) + "/" + briefs[b].file);
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
  bool indexOk = false;
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP || ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("COMPOUND", "Low heap for sync");
  } else if (HttpDownloader::downloadToFile(feedUrl + "index.txt", INDEX_TMP_PATH, nullptr, nullptr, "", "", headers) ==
             HttpDownloader::OK) {
    indexOk = true;
    std::string text;
    Storage.readFileToString("COMPOUND", INDEX_TMP_PATH, MAX_INDEX_BYTES, text);
    std::vector<IndexEntry> wanted = parseIndex(text);
    // Hashes from the previous sync: a brief whose hash changed is downloaded again.
    std::string oldText;
    if (Storage.exists(INDEX_PATH)) Storage.readFileToString("COMPOUND", INDEX_PATH, MAX_INDEX_BYTES, oldText);
    const std::vector<IndexEntry> previous = parseIndex(oldText);
    for (const auto& entry : wanted) {
      const std::string dest = std::string(DIR) + "/" + entry.file;
      const auto old = std::find_if(previous.begin(), previous.end(),
                                    [&](const IndexEntry& e) { return e.file == entry.file; });
      const bool changed = old == previous.end() || old->hash != entry.hash;
      if (Storage.exists(dest.c_str()) && !changed) continue;
      if (HttpDownloader::downloadToFile(feedUrl + entry.file, dest, nullptr, nullptr, "", "", headers) ==
          HttpDownloader::OK) {
        ++added;
      } else {
        ++failed;
        LOG_ERR("COMPOUND", "Download failed: %s", entry.file.c_str());
      }
    }
    Storage.replaceFile(INDEX_TMP_PATH, INDEX_PATH);
    // Drop briefs that left the feed (the feed keeps the newest ones).
    for (const auto& name : Storage.listFiles(DIR, 200)) {
      const std::string file = name.c_str();
      if (safeFileName(file) && std::none_of(wanted.begin(), wanted.end(),
                                             [&](const IndexEntry& e) { return e.file == file; })) {
        Storage.remove((std::string(DIR) + "/" + file).c_str());
      }
    }
  } else {
    LOG_ERR("COMPOUND", "Index download failed (HTTP %d)", HttpDownloader::lastStatus());
  }
  stopWifi();

  {
    RenderLock lock(*this);
    syncing = false;
    loadBriefs();
    if (!indexOk) {
      const int status = HttpDownloader::lastStatus();
      syncStatus = status == 401 || status == 403 || status == 404 ? "Feed refused (HTTP " + std::to_string(status) +
                                                                         "): check url and token"
                                                                   : "Sync failed: no connection to the feed";
    } else if (failed > 0) {
      syncStatus = std::to_string(added) + " new, " + std::to_string(failed) + " failed";
    } else {
      syncStatus = added == 0 ? "Up to date" : std::to_string(added) + " new or updated briefs";
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
