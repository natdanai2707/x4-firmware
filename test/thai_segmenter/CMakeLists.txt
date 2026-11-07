add_executable(ThaiSegmenterTest
  ThaiSegmenterTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ThaiSegmenter.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ThaiSegmenterTest PRIVATE ${REPO_ROOT}/lib/Utf8)
target_compile_definitions(ThaiSegmenterTest PRIVATE THAI_DICT_PATH="${REPO_ROOT}/data/thai/words.dic")

target_link_libraries(ThaiSegmenterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ThaiSegmenterTest)
