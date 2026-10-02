add_executable(IdlePageMapTest
  IdlePageMapTest.cpp
)

target_include_directories(IdlePageMapTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(IdlePageMapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IdlePageMapTest)
