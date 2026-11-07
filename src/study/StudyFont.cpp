#include "StudyFont.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#if CROSSPOINT_VECTOR_FONTS
#include <SdCardFontRegistry.h>
#include <TtfEpdFont.h>
#endif

namespace {
constexpr const char* CJK_PATHS[] = {"/study/font.ttf", "/study/font.otf"};
constexpr const char* THAI_PATHS[] = {"/study/thai.ttf", "/study/thai.otf"};
constexpr uint16_t FRONT_POINT_SIZE = 28;
constexpr uint16_t BODY_POINT_SIZE = 12;
// Fixed ids, salted apart from the reader and UI font id spaces.
constexpr int CJK_FRONT_ID = 0x53545546;   // "STUF"
constexpr int CJK_BODY_ID = 0x53545542;    // "STUB"
constexpr int THAI_FRONT_ID = 0x53545446;  // "STTF"
constexpr int THAI_BODY_ID = 0x53545442;   // "STTB"
}  // namespace

StudyFont::StudyFont(GfxRenderer& renderer, const Script script) : renderer(renderer), script(script) {}

bool StudyFont::needsThai(const std::string& text) {
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (static_cast<uint8_t>(text[i]) == 0xE0 &&
        (static_cast<uint8_t>(text[i + 1]) == 0xB8 || static_cast<uint8_t>(text[i + 1]) == 0xB9)) {
      return true;
    }
  }
  return false;
}

StudyFont::~StudyFont() { unload(); }

bool StudyFont::needsFont(const std::string& text) {
  // UTF-8 lead bytes E2 BA..EF cover U+2E80 and above (CJK radicals, kana,
  // ideographs, Hangul, full-width forms); E3..E9 hold the common ideographs.
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    const auto b0 = static_cast<uint8_t>(text[i]);
    if (b0 >= 0xE3 && b0 <= 0xED) return true;
    if (b0 == 0xEF && static_cast<uint8_t>(text[i + 1]) >= 0xBC) return true;  // full-width forms
  }
  return false;
}

bool StudyFont::load() {
#if CROSSPOINT_VECTOR_FONTS
  if (available()) return true;
  const char* path = nullptr;
  const bool thai = script == Script::Thai;
  for (const char* candidate : thai ? THAI_PATHS : CJK_PATHS) {
    if (Storage.exists(candidate)) {
      path = candidate;
      break;
    }
  }
  if (!path) return false;

  file = Storage.open(path);
  if (!file || file.size() == 0) {
    LOG_ERR("STUDY", "Cannot open %s", path);
    unload();
    return false;
  }
  size = static_cast<unsigned long>(file.size());

  front = makeUniqueNoThrow<TtfEpdFont>();
  body = makeUniqueNoThrow<TtfEpdFont>();
  if (!front || !body) {
    LOG_ERR("STUDY", "OOM: study font");
    unload();
    return false;
  }
  // Streamed from SD: only the glyphs a card uses are ever rasterized.
  front->addStreamSource(TtfEpdFont::Regular, &SdCardFontRegistry::halFileRead, &file, size);
  body->addStreamSource(TtfEpdFont::Regular, &SdCardFontRegistry::halFileRead, &file, size);
  if (!front->load(FRONT_POINT_SIZE, /*twoBit=*/true, /*glyphCacheBytes=*/48 * 1024, /*maxGlyphs=*/128) ||
      !body->load(BODY_POINT_SIZE, /*twoBit=*/true, /*glyphCacheBytes=*/32 * 1024, /*maxGlyphs=*/384)) {
    LOG_ERR("STUDY", "Cannot parse %s", path);
    unload();
    return false;
  }
  frontId = thai ? THAI_FRONT_ID : CJK_FRONT_ID;
  bodyId = thai ? THAI_BODY_ID : CJK_BODY_ID;
  renderer.insertFont(frontId, front->family());
  renderer.registerTtfFont(frontId, front.get());
  renderer.insertFont(bodyId, body->family());
  renderer.registerTtfFont(bodyId, body.get());
  LOG_INF("STUDY", "Study font loaded from %s (%lu KB)", path, size / 1024);
  return true;
#else
  return false;
#endif
}

void StudyFont::unload() {
#if CROSSPOINT_VECTOR_FONTS
  if (frontId != 0) {
    renderer.unregisterTtfFont(frontId);
    renderer.removeFont(frontId);
  }
  if (bodyId != 0) {
    renderer.unregisterTtfFont(bodyId);
    renderer.removeFont(bodyId);
  }
  front.reset();
  body.reset();
  if (file) file.close();
  size = 0;
#endif
  frontId = 0;
  bodyId = 0;
}
