add_executable(WifiTransferPolicyTest
  WifiTransferPolicyTest.cpp
)

target_include_directories(WifiTransferPolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(WifiTransferPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WifiTransferPolicyTest)
