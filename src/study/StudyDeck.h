#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "study/SrsScheduler.h"

// One study deck on the SD card:
//   /study/<name>.tsv  cards, one per line, fields separated by tabs.
//                      Field 1 is the front, the rest show on the back.
//                      Empty lines and lines starting with '#' are skipped.
//                      Add new cards at the end: progress is matched by
//                      card order.
//   /study/<name>.srs  progress, written by the device.
class StudyDeck {
 public:
  static constexpr size_t MAX_CARDS = 20000;
  static constexpr size_t MAX_LINE_BYTES = 1024;
  static constexpr size_t MAX_FIELDS = 6;

  explicit StudyDeck(std::string name) : name_(std::move(name)) {}

  // Index the card file and load progress. False when the deck file is missing
  // or has no cards.
  bool load();

  size_t cardCount() const { return offsets_.size(); }
  const std::string& tsvPath() const { return tsvPath_; }

  // Fields of one card (front first). False on a read error.
  bool readCard(uint16_t index, std::vector<std::string>& fields) const;

  std::vector<srs::CardState>& states() { return states_; }
  const std::vector<srs::CardState>& states() const { return states_; }

  // New cards already introduced on `today`.
  uint16_t newIntroducedOn(srs::Day today) const { return newDay_ == today ? newToday_ : 0; }
  void noteNewIntroduced(srs::Day today);

  // Write progress atomically (temp file, then replace).
  bool save() const;

  // Today as a local Day, or 0 when the clock is not set.
  static srs::Day today();

  // Split one TSV line into fields (exposed for tests).
  static void splitFields(const char* line, size_t length, std::vector<std::string>& fields);

 private:
  bool indexCards();
  void loadProgress();

  std::string name_;
  std::string tsvPath_;
  std::string srsPath_;
  std::vector<uint32_t> offsets_;  // file offset of each card line
  std::vector<srs::CardState> states_;
  srs::Day newDay_ = 0;
  uint16_t newToday_ = 0;
};
