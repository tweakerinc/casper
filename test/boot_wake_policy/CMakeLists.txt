add_executable(BootWakePolicyTest
  BootWakePolicyTest.cpp
)

target_include_directories(BootWakePolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(BootWakePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BootWakePolicyTest)
