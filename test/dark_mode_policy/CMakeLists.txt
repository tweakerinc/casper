add_executable(DarkModePolicyTest
  DarkModePolicyTest.cpp
)

target_include_directories(DarkModePolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(DarkModePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DarkModePolicyTest)
