#pragma once

#include <VectorFontSupport.h>
#include <HalStorage.h>

#include <memory>
#include <string>

class GfxRenderer;
class TtfEpdFont;

// Optional vector fonts for study card text the built-in fonts cannot draw,
// independent of the reader font setting:
//   Cjk:  /study/font.ttf  (Chinese)
//   Thai: /study/thai.ttf  (Thai translations)
// TrueType outlines only (.otf works only if CFF support is built in). Needs
// vector font support (PSRAM boards); elsewhere available() stays false and
// cards fall back to the built-in fonts.
class StudyFont {
 public:
  enum class Script : uint8_t { Cjk, Thai };

  StudyFont(GfxRenderer& renderer, Script script);
  ~StudyFont();
  StudyFont(const StudyFont&) = delete;
  StudyFont& operator=(const StudyFont&) = delete;

  // Open the font file and build the two sizes. Safe to call when no file exists.
  bool load();
  bool available() const { return frontId != 0; }
  int frontFontId() const { return frontId; }
  int bodyFontId() const { return bodyId; }

  // True when text has CJK characters the built-in fonts lack.
  static bool needsFont(const std::string& text);
  // True when text has Thai characters.
  static bool needsThai(const std::string& text);

 private:
  void unload();

  GfxRenderer& renderer;
  const Script script;
  int frontId = 0;
  int bodyId = 0;
#if CROSSPOINT_VECTOR_FONTS
  std::unique_ptr<TtfEpdFont> front;
  std::unique_ptr<TtfEpdFont> body;
  HalFile file;           // kept open: glyphs are read from SD on demand
  unsigned long size = 0;
#endif
};
