add_executable(SleepHoldPolicyTest
  SleepHoldPolicyTest.cpp
)

target_include_directories(SleepHoldPolicyTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(SleepHoldPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepHoldPolicyTest)
