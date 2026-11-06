#include "StudyDeck.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <ctime>

namespace {
constexpr char STUDY_DIR[] = "/study";
constexpr char MAGIC[4] = {'S', 'R', 'S', '1'};
constexpr uint16_t VERSION = 1;

struct ProgressHeader {
  char magic[4];
  uint16_t version;
  uint16_t reserved;
  uint32_t cardCount;
  uint16_t newDay;
  uint16_t newToday;
};
static_assert(sizeof(ProgressHeader) == 16, "fixed on-disk header");
}  // namespace

bool StudyDeck::load() {
  tsvPath_ = std::string(STUDY_DIR) + "/" + name_ + ".tsv";
  srsPath_ = std::string(STUDY_DIR) + "/" + name_ + ".srs";
  if (!indexCards() || offsets_.empty()) return false;
  loadProgress();
  return true;
}

bool StudyDeck::indexCards() {
  offsets_.clear();
  HalFile file;
  if (!Storage.openFileForRead("STUDY", tsvPath_, file)) return false;

  char buf[512];
  uint32_t pos = 0;
  bool atLineStart = true;
  uint32_t lineStart = 0;
  bool skipBom = true;
  int got;
  while ((got = file.read(buf, sizeof(buf))) > 0) {
    for (int i = 0; i < got; ++i, ++pos) {
      const char c = buf[i];
      if (skipBom) {
        // A UTF-8 BOM (EF BB BF) is not part of the first card.
        if (pos < 3 && static_cast<uint8_t>(c) == (pos == 0 ? 0xEF : pos == 1 ? 0xBB : 0xBF)) {
          if (pos == 2) {
            skipBom = false;
            lineStart = 3;
          }
          continue;
        }
        skipBom = false;
      }
      if (atLineStart) {
        atLineStart = false;
        lineStart = pos;
        if (c != '\n' && c != '\r' && c != '#' && offsets_.size() < MAX_CARDS) offsets_.push_back(lineStart);
      }
      if (c == '\n') atLineStart = true;
    }
  }
  file.close();
  LOG_INF("STUDY", "%s: %u cards", tsvPath_.c_str(), static_cast<unsigned>(offsets_.size()));
  return true;
}

void StudyDeck::loadProgress() {
  states_.assign(offsets_.size(), srs::CardState{});
  newDay_ = 0;
  newToday_ = 0;

  HalFile file;
  if (!Storage.exists(srsPath_.c_str()) || !Storage.openFileForRead("STUDY", srsPath_, file)) return;
  ProgressHeader header{};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
      std::memcmp(header.magic, MAGIC, sizeof(MAGIC)) != 0 || header.version != VERSION) {
    LOG_ERR("STUDY", "Ignoring unreadable progress file %s", srsPath_.c_str());
    file.close();
    return;
  }
  // Cards added to the deck since the last save start as new.
  const size_t stored = std::min<size_t>(header.cardCount, states_.size());
  const size_t bytes = stored * sizeof(srs::CardState);
  if (bytes > 0 && file.read(states_.data(), bytes) != static_cast<int>(bytes)) {
    LOG_ERR("STUDY", "Short progress file %s", srsPath_.c_str());
    states_.assign(offsets_.size(), srs::CardState{});
    file.close();
    return;
  }
  newDay_ = header.newDay;
  newToday_ = header.newToday;
  file.close();
}

void StudyDeck::noteNewIntroduced(const srs::Day today) {
  if (newDay_ != today) {
    newDay_ = today;
    newToday_ = 0;
  }
  if (newToday_ < UINT16_MAX) ++newToday_;
}

bool StudyDeck::save() const {
  Storage.ensureDirectoryExists(STUDY_DIR);
  const std::string tmpPath = srsPath_ + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("STUDY", tmpPath, file)) return false;

  ProgressHeader header{};
  std::memcpy(header.magic, MAGIC, sizeof(MAGIC));
  header.version = VERSION;
  header.cardCount = static_cast<uint32_t>(states_.size());
  header.newDay = newDay_;
  header.newToday = newToday_;
  const size_t bytes = states_.size() * sizeof(srs::CardState);
  const bool ok = file.write(&header, sizeof(header)) == sizeof(header) &&
                  (bytes == 0 || file.write(states_.data(), bytes) == bytes);
  file.close();
  if (!ok || !Storage.replaceFile(tmpPath.c_str(), srsPath_.c_str())) {
    LOG_ERR("STUDY", "Failed to save %s", srsPath_.c_str());
    return false;
  }
  return true;
}

bool StudyDeck::readCard(const uint16_t index, std::vector<std::string>& fields) const {
  fields.clear();
  if (index >= offsets_.size()) return false;
  HalFile file;
  if (!Storage.openFileForRead("STUDY", tsvPath_, file) || !file.seek(offsets_[index])) return false;

  char line[MAX_LINE_BYTES];
  const int got = file.read(line, sizeof(line));
  file.close();
  if (got <= 0) return false;
  size_t length = 0;
  while (length < static_cast<size_t>(got) && line[length] != '\n') ++length;
  splitFields(line, length, fields);
  return !fields.empty();
}

void StudyDeck::splitFields(const char* line, size_t length, std::vector<std::string>& fields) {
  fields.clear();
  if (length > 0 && line[length - 1] == '\r') --length;
  size_t start = 0;
  for (size_t i = 0; i <= length && fields.size() < MAX_FIELDS; ++i) {
    if (i == length || line[i] == '\t') {
      fields.emplace_back(line + start, i - start);
      start = i + 1;
    }
  }
  while (!fields.empty() && fields.back().empty()) fields.pop_back();
}

srs::Day StudyDeck::today() {
  const time_t now = time(nullptr);
  if (now < 1577836800) return 0;  // clock not set (before 2020)
  struct tm local {};
  localtime_r(&now, &local);
  return srs::dayFromDate(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}
