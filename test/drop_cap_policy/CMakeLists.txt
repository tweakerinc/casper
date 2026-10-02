add_executable(DropCapPolicyTest
  DropCapPolicyTest.cpp
)

target_include_directories(DropCapPolicyTest PRIVATE
  ${REPO_ROOT}/lib/Rivulet
)

target_link_libraries(DropCapPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DropCapPolicyTest)
