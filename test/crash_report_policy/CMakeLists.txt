add_executable(CrashReportPolicyTest
  CrashReportPolicyTest.cpp
)

target_include_directories(CrashReportPolicyTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(CrashReportPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CrashReportPolicyTest)
