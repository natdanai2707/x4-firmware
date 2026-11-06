#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// Compound briefs on the device. The Compound scheduled tasks publish each
// brief as an EPUB plus an index.txt to a feed URL; this screen syncs that
// feed into /compound/ over WiFi and opens a brief in the reader.
//
// Setup: /compound/feed.txt with
//   url=https://raw.githubusercontent.com/<owner>/<repo>/main/feed/
//   token=<optional GitHub token for a private repo>
class CompoundActivity final : public UiListActivity {
 public:
  explicit CompoundActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  bool preventAutoSleep() override { return syncing; }

 private:
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

  std::string feedUrl;
  std::string token;
  std::vector<Brief> briefs;
  std::vector<freeink::ui::ListItem> rows;  // row 0 is Sync, then briefs
  std::string syncLabel;
  std::string syncStatus;
  bool syncing = false;
};
