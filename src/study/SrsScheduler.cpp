#include "SrsScheduler.h"

#include <algorithm>

namespace srs {

namespace {
uint16_t clampInterval(uint32_t days) {
  if (days < 1) return 1;
  return static_cast<uint16_t>(std::min<uint32_t>(days, MAX_INTERVAL));
}

Day addDays(Day today, uint16_t days) {
  const uint32_t due = static_cast<uint32_t>(today) + days;
  return static_cast<Day>(std::min<uint32_t>(due, UINT16_MAX));
}
}  // namespace

CardState review(CardState s, const Grade grade, const Day today) {
  if (isNew(s)) {
    s.ease = START_EASE;
    switch (grade) {
      case Grade::Again:
        s.interval = 0;
        s.due = today;
        break;
      case Grade::Good:
        s.interval = 1;
        s.reps = 1;
        s.due = addDays(today, s.interval);
        break;
      case Grade::Easy:
        s.interval = 3;
        s.reps = 1;
        s.ease = START_EASE + 15;
        s.due = addDays(today, s.interval);
        break;
    }
    return s;
  }

  switch (grade) {
    case Grade::Again:
      if (s.reps > 0) s.lapses = static_cast<uint8_t>(std::min(s.lapses + 1, 255));
      s.reps = 0;
      s.ease = static_cast<uint16_t>(std::max<int>(s.ease - 20, MIN_EASE));
      s.interval = 0;
      s.due = today;
      break;
    case Grade::Good: {
      // A card relearned after Again restarts at one day.
      const uint32_t grown = s.interval == 0 ? 1 : (static_cast<uint32_t>(s.interval) * s.ease + 50) / 100;
      s.interval = clampInterval(std::max<uint32_t>(grown, s.interval + 1u));
      s.reps = static_cast<uint8_t>(std::min(s.reps + 1, 255));
      s.due = addDays(today, s.interval);
      break;
    }
    case Grade::Easy: {
      const uint32_t base = s.interval == 0 ? 2 : s.interval;
      const uint32_t grown = (base * s.ease * 13 + 500) / 1000;  // ease x 1.3
      s.interval = clampInterval(std::max<uint32_t>(grown, base + 1u));
      s.ease = static_cast<uint16_t>(s.ease + 15);
      s.reps = static_cast<uint8_t>(std::min(s.reps + 1, 255));
      s.due = addDays(today, s.interval);
      break;
    }
  }
  return s;
}

std::vector<uint16_t> planSession(const std::vector<CardState>& states, const Day today, const SessionLimits& limits,
                                  const uint16_t newAlreadyToday) {
  std::vector<uint16_t> due;
  std::vector<uint16_t> fresh;
  const uint16_t newBudget = newAlreadyToday >= limits.newPerDay ? 0 : limits.newPerDay - newAlreadyToday;
  const size_t count = std::min<size_t>(states.size(), UINT16_MAX);

  for (size_t i = 0; i < count; ++i) {
    const CardState& s = states[i];
    if (isNew(s)) {
      if (fresh.size() < newBudget) fresh.push_back(static_cast<uint16_t>(i));
    } else if (s.due <= today) {
      due.push_back(static_cast<uint16_t>(i));
    }
  }

  // Most overdue first; deck order breaks ties so the plan is stable.
  std::stable_sort(due.begin(), due.end(), [&states](uint16_t a, uint16_t b) { return states[a].due < states[b].due; });
  if (due.size() > limits.maxReviews) due.resize(limits.maxReviews);

  due.insert(due.end(), fresh.begin(), fresh.end());
  return due;
}

Day dayFromDate(const int year, const int month, const int dayOfMonth) {
  // Days from civil date (Howard Hinnant's algorithm).
  const int y = year - (month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2u) / 5u +
                       static_cast<unsigned>(dayOfMonth) - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
  if (days < 0) return 0;
  return static_cast<Day>(std::min<long>(days, UINT16_MAX));
}

}  // namespace srs
