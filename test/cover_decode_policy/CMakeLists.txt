add_executable(CoverDecodePolicyTest
  CoverDecodePolicyTest.cpp
)

target_include_directories(CoverDecodePolicyTest PRIVATE
  ${REPO_ROOT}/lib/JpegToBmpConverter
)

target_link_libraries(CoverDecodePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CoverDecodePolicyTest)
