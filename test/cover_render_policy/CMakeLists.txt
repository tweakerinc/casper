add_executable(CoverRenderPolicyTest
  CoverRenderPolicyTest.cpp
)

target_include_directories(CoverRenderPolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(CoverRenderPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CoverRenderPolicyTest)
