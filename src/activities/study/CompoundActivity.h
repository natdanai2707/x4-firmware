#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Compound briefs on the device. The Compound scheduled tasks publish each
// brief (EPUB and plain text .cb) plus an index.txt to a feed URL. This
// screen syncs the .cb files over WiFi, once a day by itself
// after 07:00 when a WiFi network is saved, and opens a brief in
// CompoundBriefActivity. Words from new briefs join the IELTS deck.
//
// Files live in the hidden /.crosspoint/compound/ and only the last 3 days are
// kept. The feed url is built in; /.crosspoint/compound/feed.txt may override
//   url=https://raw.githubusercontent.com/<owner>/<repo>/main/feed/
//   token=<optional GitHub token for a private repo>
class CompoundActivity final : public UiListActivity {
 public:
  explicit CompoundActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return syncing; }

 private:
  struct IndexEntry {
    std::string file;
    std::string date;
    std::string hash;
  };
  static std::vector<IndexEntry> parseIndex(const std::string& text);
  static void keepRecent(std::vector<IndexEntry>& entries);
  static void pruneFiles(const std::vector<IndexEntry>& keep);
  static void migrateLegacy();

  struct Brief {
    std::string file;
    std::string type;
    std::string date;
    std::string title;
    std::string subtitle;
  };

  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override { return "Compound"; }

  bool readConfig();
  void loadBriefs();
  void rebuildRows();
  void startSync();
  void runSync();
  void stopWifi();
  bool shouldAutoSync();
  int addWordsToVocab(const std::vector<std::string>& briefPaths);

  std::string feedUrl;
  std::string token;
  std::vector<Brief> briefs;
  std::vector<freeink::ui::ListItem> rows;  // row 0 is Sync, then briefs
  std::string syncLabel;
  std::string syncStatus;
  bool syncing = false;
  bool autoSyncPending = false;
};
