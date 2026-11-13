#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// App-style home: clock, a Continue reading card and a grid of study apps
// that open directly. Device tools (Browse Files, Library, File Transfer,
// Settings) live in a drawer that slides in from the right edge; the Back
// button also opens and closes it.
class LauncherActivity final : public Activity {
 public:
  LauncherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  enum class App : uint8_t { Continue, Compound, Vocab, Chinese, Library };
  enum class Tool : uint8_t { Files, Library, Transfer, Settings };
  static constexpr int APP_COUNT = 5;  // Continue card + 4 tiles
  static constexpr int TOOL_COUNT = 4;

  void openApp(App app);
  void openTool(Tool tool);
  void setDrawer(bool open);
  int tileAt(int x, int y) const;  // App index or -1
  int toolAt(int x, int y) const;  // Tool index or -1

  // Layout, shared by render() and touch hit tests.
  int cardTop() const;
  int cardHeight() const { return cardH; }
  int gridTop() const { return cardTop() + cardHeight() + SECTION_GAP; }
  int tileSize() const;
  int tileHeight() const;
  void measureCard();  // sets cardH from the book title and author

  static constexpr int INSET = 18;        // padding inside cards and tiles
  static constexpr int SECTION_GAP = 24;  // space between blocks
  int cardH = 120;
  std::vector<std::string> titleLines;
  int drawerLeft() const;

  std::string bookTitle;
  std::string bookPath;
  std::string bookAuthor;
  int selected = 0;  // App index; 0 is the Continue card
  bool drawerOpen = false;
  int drawerSelected = 0;
};
