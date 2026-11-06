#pragma once

#include <VectorFontSupport.h>
#include <HalStorage.h>

#include <memory>
#include <string>

class GfxRenderer;
class TtfEpdFont;

// Optional vector font for study cards, read from /study/font.ttf (TrueType outlines; .otf only if CFF support is built in).
// Used for card text the built-in fonts cannot draw (Chinese). Independent of
// the reader font setting. Needs vector font support (PSRAM boards); on other
// boards available() stays false and cards fall back to the built-in fonts.
class StudyFont {
 public:
  explicit StudyFont(GfxRenderer& renderer);
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

 private:
  void unload();

  GfxRenderer& renderer;
  int frontId = 0;
  int bodyId = 0;
#if CROSSPOINT_VECTOR_FONTS
  std::unique_ptr<TtfEpdFont> front;
  std::unique_ptr<TtfEpdFont> body;
  HalFile file;           // kept open: glyphs are read from SD on demand
  unsigned long size = 0;
#endif
};
