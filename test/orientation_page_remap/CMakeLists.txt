add_executable(OrientationPageRemapTest
  OrientationPageRemapTest.cpp
)

target_include_directories(OrientationPageRemapTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(OrientationPageRemapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OrientationPageRemapTest)
