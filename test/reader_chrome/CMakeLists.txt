add_executable(ReaderChromePolicyTest
  ReaderChromePolicyTest.cpp
)

target_include_directories(ReaderChromePolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(ReaderChromePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderChromePolicyTest)
