add_executable(FatDateTimePolicyTest
  FatDateTimePolicyTest.cpp
)

target_include_directories(FatDateTimePolicyTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(FatDateTimePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FatDateTimePolicyTest)
