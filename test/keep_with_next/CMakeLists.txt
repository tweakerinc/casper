add_executable(KeepWithNextPolicyTest
  KeepWithNextPolicyTest.cpp
)

target_include_directories(KeepWithNextPolicyTest PRIVATE
  ${REPO_ROOT}/lib/Rivulet
)

target_link_libraries(KeepWithNextPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KeepWithNextPolicyTest)
