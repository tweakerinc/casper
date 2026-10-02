add_executable(ThumbCachePolicyTest
  ThumbCachePolicyTest.cpp
)

target_include_directories(ThumbCachePolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(ThumbCachePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ThumbCachePolicyTest)
