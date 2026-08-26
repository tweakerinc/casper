add_executable(InputPollPolicyTest
  InputPollPolicyTest.cpp
)

target_include_directories(InputPollPolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(InputPollPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InputPollPolicyTest)
