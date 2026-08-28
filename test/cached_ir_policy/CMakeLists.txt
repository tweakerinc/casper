add_executable(CachedIrPolicyTest
  CachedIrPolicyTest.cpp
)

target_include_directories(CachedIrPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CachedIrPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CachedIrPolicyTest)
