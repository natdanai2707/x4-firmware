#include <gtest/gtest.h>

#include "study/SrsScheduler.h"

using srs::CardState;
using srs::Grade;

namespace {
constexpr srs::Day TODAY = 20763;
}

TEST(SrsScheduler, DayFromDate) {
  EXPECT_EQ(srs::dayFromDate(1970, 1, 1), 0);
  EXPECT_EQ(srs::dayFromDate(2000, 3, 1), 11017);
  EXPECT_EQ(srs::dayFromDate(2026, 11, 6), 20763);
  EXPECT_EQ(srs::dayFromDate(2026, 12, 31) + 1, srs::dayFromDate(2027, 1, 1));
}

TEST(SrsScheduler, NewCardGood) {
  const CardState s = srs::review(CardState{}, Grade::Good, TODAY);
  EXPECT_FALSE(srs::isNew(s));
  EXPECT_EQ(s.interval, 1);
  EXPECT_EQ(s.due, TODAY + 1);
  EXPECT_EQ(s.ease, srs::START_EASE);
  EXPECT_EQ(s.reps, 1);
}

TEST(SrsScheduler, NewCardEasy) {
  const CardState s = srs::review(CardState{}, Grade::Easy, TODAY);
  EXPECT_EQ(s.interval, 3);
  EXPECT_EQ(s.due, TODAY + 3);
  EXPECT_GT(s.ease, srs::START_EASE);
}

TEST(SrsScheduler, NewCardAgainStaysDueTodayThenOneDay) {
  CardState s = srs::review(CardState{}, Grade::Again, TODAY);
  EXPECT_FALSE(srs::isNew(s));
  EXPECT_EQ(s.due, TODAY);
  EXPECT_EQ(s.lapses, 0);  // never learned, so not a lapse
  s = srs::review(s, Grade::Good, TODAY);
  EXPECT_EQ(s.interval, 1);
  EXPECT_EQ(s.due, TODAY + 1);
}

TEST(SrsScheduler, GoodGrowsByEase) {
  CardState s = srs::review(CardState{}, Grade::Good, TODAY);  // 1 day
  s = srs::review(s, Grade::Good, s.due);                       // round(1 x 2.5) = 3
  EXPECT_EQ(s.interval, 3);
  s = srs::review(s, Grade::Good, s.due);  // round(3*2.5)=8
  EXPECT_EQ(s.interval, 8);
  s = srs::review(s, Grade::Good, s.due);  // 20
  EXPECT_EQ(s.interval, 20);
  EXPECT_EQ(s.reps, 4);
}

TEST(SrsScheduler, AgainOnLearnedCardIsALapse) {
  CardState s = srs::review(CardState{}, Grade::Good, TODAY);
  s = srs::review(s, Grade::Good, s.due);
  const srs::Day day = s.due;
  s = srs::review(s, Grade::Again, day);
  EXPECT_EQ(s.lapses, 1);
  EXPECT_EQ(s.reps, 0);
  EXPECT_EQ(s.due, day);
  EXPECT_EQ(s.ease, srs::START_EASE - 20);
  s = srs::review(s, Grade::Good, day);
  EXPECT_EQ(s.interval, 1);
}

TEST(SrsScheduler, EaseNeverBelowMinimum) {
  CardState s = srs::review(CardState{}, Grade::Good, TODAY);
  for (int i = 0; i < 20; ++i) s = srs::review(s, Grade::Again, TODAY);
  EXPECT_EQ(s.ease, srs::MIN_EASE);
}

TEST(SrsScheduler, IntervalIsCapped) {
  CardState s;
  s.ease = 300;
  s.interval = 3000;
  s.reps = 5;
  s = srs::review(s, Grade::Easy, TODAY);
  EXPECT_EQ(s.interval, srs::MAX_INTERVAL);
}

TEST(SrsScheduler, PlanPutsOverdueFirstThenNew) {
  std::vector<CardState> states(6);
  states[1] = srs::review(CardState{}, Grade::Good, TODAY - 5);  // due TODAY-4
  states[3] = srs::review(CardState{}, Grade::Easy, TODAY - 5);  // due TODAY-2
  states[4] = srs::review(CardState{}, Grade::Good, TODAY);      // due tomorrow
  const auto plan = srs::planSession(states, TODAY, {}, 0);
  ASSERT_EQ(plan.size(), 5u);
  EXPECT_EQ(plan[0], 1);
  EXPECT_EQ(plan[1], 3);
  EXPECT_EQ(plan[2], 0);  // new cards in deck order
  EXPECT_EQ(plan[3], 2);
  EXPECT_EQ(plan[4], 5);
}

TEST(SrsScheduler, PlanRespectsNewBudget) {
  std::vector<CardState> states(30);
  srs::SessionLimits limits;
  limits.newPerDay = 10;
  EXPECT_EQ(srs::planSession(states, TODAY, limits, 0).size(), 10u);
  EXPECT_EQ(srs::planSession(states, TODAY, limits, 7).size(), 3u);
  EXPECT_EQ(srs::planSession(states, TODAY, limits, 12).size(), 0u);
}

TEST(SrsScheduler, PlanCapsReviews) {
  std::vector<CardState> states(50, srs::review(CardState{}, Grade::Good, TODAY - 3));
  srs::SessionLimits limits;
  limits.maxReviews = 20;
  EXPECT_EQ(srs::planSession(states, TODAY, limits, 0).size(), 20u);
}
