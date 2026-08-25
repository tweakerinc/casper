add_executable(QrSleepPanelPolicyTest
  QrSleepPanelPolicyTest.cpp
)

target_include_directories(QrSleepPanelPolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(QrSleepPanelPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(QrSleepPanelPolicyTest)
