add_executable(SrsSchedulerTest
  SrsSchedulerTest.cpp
  ${REPO_ROOT}/src/study/SrsScheduler.cpp
)

target_include_directories(SrsSchedulerTest PRIVATE ../../src)

target_link_libraries(SrsSchedulerTest PRIVATE GTest::gtest_main)

gtest_discover_tests(SrsSchedulerTest)
