add_executable(PageTurnPolicyTest
  PageTurnPolicyTest.cpp
)

target_include_directories(PageTurnPolicyTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(PageTurnPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PageTurnPolicyTest)
