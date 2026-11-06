#pragma once

#include <cstdint>
#include <vector>

// Spaced repetition core shared by the IELTS/TOEFL and Chinese decks.
// Pure C++ (no Arduino or storage), so it runs in host unit tests.
namespace srs {

// Days are counted from 1970-01-01 in local time.
using Day = uint16_t;

// Per-card schedule. 8 bytes, stored as-is in the deck's .srs file.
struct CardState {
  Day due = 0;            // first day the card is due again
  uint16_t interval = 0;  // days between the last two reviews
  uint16_t ease = 0;      // interval multiplier x100 (250 = 2.5x); 0 means new
  uint8_t reps = 0;       // successful reviews in a row
  uint8_t lapses = 0;     // times the card was forgotten after being learned
};
static_assert(sizeof(CardState) == 8, "CardState is a fixed on-disk record");

enum class Grade : uint8_t { Again, Good, Easy };

constexpr uint16_t START_EASE = 250;
constexpr uint16_t MIN_EASE = 130;
constexpr uint16_t MAX_INTERVAL = 3650;

inline bool isNew(const CardState& s) { return s.ease == 0; }

// Apply one answer given on `today` and return the new state. Again keeps
// the card due today so the session shows it again.
CardState review(CardState s, Grade grade, Day today);

struct SessionLimits {
  uint16_t maxReviews = 100;
  uint16_t newPerDay = 10;
};

// Cards for one session: due reviews first (most overdue first), then new
// cards in deck order. `newAlreadyToday` counts new cards already introduced
// today, so a second session the same day does not exceed newPerDay.
std::vector<uint16_t> planSession(const std::vector<CardState>& states, Day today, const SessionLimits& limits,
                                  uint16_t newAlreadyToday);

// Local calendar date to Day.
Day dayFromDate(int year, int month, int dayOfMonth);

}  // namespace srs
