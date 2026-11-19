#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// App-style home: clock, a Today card with what is due in each app, a
// Continue reading card and a row of apps that open directly. Device tools
// (Browse Files, Library, File Transfer, Settings) live in a drawer that
// slides in from the right edge; the menu button and Back also open it.
class LauncherActivity final : public Activity {
 public:
  LauncherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  enum class App : uint8_t { Continue, Compound, Vocab, Chinese, Training };
  enum class Tool : uint8_t { Files, Library, Transfer, Settings };
  static constexpr int APP_COUNT = 5;  // Continue card + 4 tiles
  static constexpr int TOOL_COUNT = 4;

  void openApp(App app);
  void openTool(Tool tool);
  void setDrawer(bool open);
  int tileAt(int x, int y) const;  // App index or -1
  int toolAt(int x, int y) const;  // Tool index or -1

  // One line of the Today card; tapping it opens `app`.
  struct TodayRow {
    App app;
    const uint8_t* icon;
    std::string label;
    std::string text;
    std::string meta;
    bool done = false;
  };
  void buildToday();

  // Layout, set by render() and used by touch hit tests.
  struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool has(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
  };
  std::vector<TodayRow> today;
  std::vector<Box> rowBoxes;
  Box menuBox;
  Box sleepBox;  // Sleep button in the device menu
  Box cardBox;
  Box tileBoxes[4];
  int drawerLeft() const;

  std::string bookTitle;
  std::string bookPath;
  std::string bookAuthor;
  int bookPercent = -1;  // percent read of the Continue book, -1 unknown
  int selected = 0;      // App index; 0 is the Continue card
  bool drawerOpen = false;
  int drawerSelected = 0;
};
