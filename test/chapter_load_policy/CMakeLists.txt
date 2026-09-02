add_executable(ChapterLoadPolicyTest
  ChapterLoadPolicyTest.cpp
)

target_include_directories(ChapterLoadPolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(ChapterLoadPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterLoadPolicyTest)
