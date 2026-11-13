#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "study/CompoundBrief.h"
#include "study/StudyFont.h"

// Compound briefs on the device, laid out like the Compound phone app: a
// bottom tab bar (Markets, Ideas, Words), the latest brief as a hero card and
// the earlier ones as a list. A brief opens in CompoundBriefActivity.
//
// The Compound scheduled tasks publish each brief (EPUB and plain text .cb)
// plus an index.txt to a feed URL. This screen syncs the .cb files over WiFi,
// once a day by itself after 07:00 when a WiFi network is saved. Words from
// new briefs join the IELTS deck.
//
// Files live in the hidden /.crosspoint/compound/ and only the last 3 days are
// kept. The feed url is built in; /.crosspoint/compound/feed.txt may override
//   url=https://raw.githubusercontent.com/<owner>/<repo>/main/feed/
//   token=<optional GitHub token for a private repo>
class CompoundActivity final : public Activity {
 public:
  explicit CompoundActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
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

  // One brief on the device. Sections are dropped after parsing to save RAM;
  // the reader loads the full file again.
  struct Brief {
    std::string path;  // the .cb file
    CompoundBrief data;
  };
  enum class Tab : uint8_t { Markets, Ideas, Words };
  static constexpr int TAB_COUNT = 3;

  bool readConfig();
  void loadBriefs();
  void startSync();
  void runSync();
  void stopWifi();
  bool shouldAutoSync();
  int addWordsToVocab(const std::vector<std::string>& briefPaths);

  std::vector<const Brief*> ofTab() const;  // newest first
  void setTab(Tab t);
  void openBrief(const Brief* b);
  void layoutWords();
  int contentTop() const;
  int tabBarTop() const;
  void renderFeed(int y);
  void renderWords(int y);
  void renderTabBar();

  std::string feedUrl;
  std::string token;
  std::vector<Brief> briefs;
  std::string syncStatus;
  bool syncing = false;
  bool autoSyncPending = false;

  Tab tab = Tab::Markets;
  int focus = 0;              // 0 hero, 1.. earlier rows
  std::vector<int> itemTops;  // hit bands of hero and rows, from the last render
  std::vector<int> itemBottoms;
  int statusTop = 0;
  int statusBottom = 0;

  // Words tab: wrapped lines, paginated.
  struct WordLine {
    int fontId;
    uint8_t style;
    int indent;
    int gapBefore;
    bool rule;
    bool bar;
    std::string text;
  };
  std::vector<WordLine> wordLines;
  std::vector<size_t> wordPages;
  int wordPage = 0;
  std::unique_ptr<StudyFont> thaiFont;
};
