add_executable(ProgressFlushPolicyTest
  ProgressFlushPolicyTest.cpp
)

target_include_directories(ProgressFlushPolicyTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ProgressFlushPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressFlushPolicyTest)
